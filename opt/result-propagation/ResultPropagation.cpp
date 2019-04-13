/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResultPropagation.h"

#include <vector>

#include "BaseIRAnalyzer.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "Resolver.h"
#include "Walkers.h"

using namespace sparta;

namespace {

constexpr const char* METRIC_METHODS_WHICH_RETURN_PARAMETER =
    "num_methods_which_return_parameters";
constexpr const char* METRIC_ERASED_MOVE_RESULTS = "num_erased_move_results";
constexpr const char* METRIC_PATCHED_MOVE_RESULTS = "num_patched_move_results";
constexpr const char* METRIC_UNVERIFIABLE_MOVE_RESULTS =
    "num_unverifiable_move_results";
constexpr const char* METRIC_METHODS_WHICH_RETURN_PARAMETER_ITERATIONS =
    "num_methods_which_return_parameters_iterations";
constexpr const ParamIndex WIDE_HIGH = 1 << 31;

IROpcode move_result_to_move(IROpcode op) {
  switch (op) {
  case OPCODE_MOVE_RESULT:
    return OPCODE_MOVE;
  case OPCODE_MOVE_RESULT_OBJECT:
    return OPCODE_MOVE_OBJECT;
  case OPCODE_MOVE_RESULT_WIDE:
    return OPCODE_MOVE_WIDE;
  default:
    always_assert(false);
  }
}

void patch_move_result_to_move(IRInstruction* move_result_inst, uint16_t reg) {
  const auto op = move_result_inst->opcode();
  move_result_inst->set_opcode(move_result_to_move(op));
  move_result_inst->set_arg_word_count(1);
  move_result_inst->set_src(0, reg);
}

const DexType* get_param_type(bool is_static,
                              const DexMethodRef* method,
                              ParamIndex param_index) {
  if (!is_static && param_index-- == 0) {
    return method->get_class();
  }
  const auto args = method->get_proto()->get_args()->get_type_list();
  return args[param_index];
}

using register_t = ir_analyzer::register_t;
using namespace ir_analyzer;

using ParamDomain = sparta::ConstantAbstractDomain<ParamIndex>;

/**
 * For each register that holds an reference to a param
 * keeps track of the param index.
 **/
using ParamDomainEnvironment =
    sparta::PatriciaTreeMapAbstractEnvironment<register_t, ParamDomain>;

// We use this special register to denote the value that is being returned.
register_t RETURN_VALUE = RESULT_REGISTER - 1;

bool isNotHigh(ParamDomain domain) {
  auto const constant = domain.get_constant();
  return !constant || (((*constant) & WIDE_HIGH) == 0);
}

ParamDomain makeHigh(ParamDomain domain) {
  always_assert(isNotHigh(domain));
  auto const constant = domain.get_constant();
  return constant ? ParamDomain((*constant) | WIDE_HIGH) : domain;
}

class Analyzer final : public BaseIRAnalyzer<ParamDomainEnvironment> {

 public:
  Analyzer(cfg::ControlFlowGraph& cfg,
           const ReturnParamResolver& resolver,
           const std::unordered_map<const DexMethod*, ParamIndex>&
               methods_which_return_parameter)
      : BaseIRAnalyzer(cfg),
        m_resolver(resolver),
        m_methods_which_return_parameter(methods_which_return_parameter),
        m_load_param_map(get_load_param_map(cfg)) {
    MonotonicFixpointIterator::run(ParamDomainEnvironment::top());
  }

  void analyze_instruction(
      IRInstruction* insn,
      ParamDomainEnvironment* current_state) const override {

    // While the special registers RESULT_REGISTER and RETURN_VALUE do not
    // participate in possible wide low and high register splitting, all
    // other registers should be accessed through the following two helper
    // functions to ensure that wide values are properly handled.

    const auto get_current_state_at = [&](register_t reg, bool wide) {
      const auto low = current_state->get(reg);
      if (!wide) {
        return isNotHigh(low) ? low : ParamDomain::top();
      }
      const auto high = current_state->get(reg + 1);
      return (isNotHigh(low) && makeHigh(low) == high) ? low
                                                       : ParamDomain::top();
    };

    const auto set_current_state_at = [&](register_t reg, bool wide,
                                          ParamDomain value) {
      always_assert(isNotHigh(value));
      current_state->set(reg, value);
      if (wide) {
        current_state->set(reg + 1, makeHigh(value));
      }
    };

    const auto default_case = [&]() {
      // If we get here, reset destination.
      if (insn->dests_size()) {
        set_current_state_at(insn->dest(), insn->dest_is_wide(),
                             ParamDomain::top());
      } else if (insn->has_move_result() || insn->has_move_result_pseudo()) {
        current_state->set(RESULT_REGISTER, ParamDomain::top());
      }
    };

    switch (insn->opcode()) {
    case OPCODE_RETURN:
    case OPCODE_RETURN_OBJECT:
    case OPCODE_RETURN_WIDE: {
      const auto value =
          get_current_state_at(insn->src(0), insn->src_is_wide(0));
      current_state->set(RETURN_VALUE, value);
      break;
    }

    case OPCODE_MOVE:
    case OPCODE_MOVE_OBJECT:
    case OPCODE_MOVE_WIDE: {
      const auto value =
          get_current_state_at(insn->src(0), insn->src_is_wide(0));
      set_current_state_at(insn->dest(), insn->dest_is_wide(), value);
      break;
    }

    case IOPCODE_MOVE_RESULT_PSEUDO:
    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
    case OPCODE_MOVE_RESULT:
    case OPCODE_MOVE_RESULT_OBJECT:
    case OPCODE_MOVE_RESULT_WIDE: {
      const auto value = current_state->get(RESULT_REGISTER);
      set_current_state_at(insn->dest(), insn->dest_is_wide(), value);
      break;
    }

    case IOPCODE_LOAD_PARAM:
    case IOPCODE_LOAD_PARAM_OBJECT:
    case IOPCODE_LOAD_PARAM_WIDE: {
      const auto param_index = m_load_param_map.at(insn);
      const auto value = ParamDomain(param_index);
      set_current_state_at(insn->dest(), insn->dest_is_wide(), value);
      break;
    }

    case OPCODE_CHECK_CAST: {
      // We track check cast like a move; this gives us the maximum information
      // across various call chains. Only when we are about to actually patch
      // the code we check whether such patching is verifiable.
      const auto value =
          get_current_state_at(insn->src(0), insn->src_is_wide(0));
      current_state->set(RESULT_REGISTER, value);
      break;
    }

    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_VIRTUAL:
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_INTERFACE:
    case OPCODE_INVOKE_SUPER: {
      // Avoid call resolution if all srcs are top anyway.
      bool all_top = true;
      for (size_t i = 0; i < insn->srcs_size(); i++) {
        const auto param_value =
            get_current_state_at(insn->src(i), insn->src_is_wide(i));
        if (!param_value.is_top()) {
          all_top = false;
          break;
        }
      }
      if (all_top) {
        default_case();
        break;
      }

      // TODO(perf): call resolution is quite expensive; figure out
      // beforehand if the result of this invoke instruction can ever flow
      // to a return instruction; if not, skip this
      const auto param_index = m_resolver.get_return_param_index(
          insn, m_methods_which_return_parameter, m_resolved_refs);
      if (!param_index) {
        default_case();
        break;
      }

      const auto param_value = get_current_state_at(
          insn->src(*param_index), insn->src_is_wide(*param_index));
      current_state->set(RESULT_REGISTER, param_value);
      break;
    }

    default: {
      default_case();
      break;
    }
    }
  }

 private:
  const ReturnParamResolver& m_resolver;
  const std::unordered_map<const DexMethod*, ParamIndex>&
      m_methods_which_return_parameter;
  const std::unordered_map<const IRInstruction*, ParamIndex> m_load_param_map;
  mutable MethodRefCache m_resolved_refs;
};
} // namespace

////////////////////////////////////////////////////////////////////////////////

const std::unordered_map<const IRInstruction*, ParamIndex> get_load_param_map(
    cfg::ControlFlowGraph& cfg) {
  std::unordered_map<const IRInstruction*, ParamIndex> map;
  const auto param_insns = InstructionIterable(cfg.get_param_instructions());
  ParamIndex index = 0;
  for (auto it = param_insns.begin(); it != param_insns.end(); it++) {
    const auto insn = it->insn;
    always_assert(opcode::is_load_param(insn->opcode()));
    map.insert({insn, index++});
  }
  return map;
}

const boost::optional<ParamIndex> ReturnParamResolver::get_return_param_index(
    IRInstruction* insn,
    const std::unordered_map<const DexMethod*, ParamIndex>&
        methods_which_return_parameter,
    MethodRefCache& resolved_refs) const {
  always_assert(is_invoke(insn->opcode()));
  const auto method = insn->get_method();
  const auto proto = method->get_proto();
  if (proto->is_void()) {
    // No point in doing any further analysis
    return boost::none;
  }

  const auto opcode = insn->opcode();
  if (opcode == OPCODE_INVOKE_VIRTUAL && returns_receiver(method)) {
    return 0;
  }

  const auto callee =
      resolve_method(method, opcode_to_search(insn), resolved_refs);
  if (callee == nullptr) {
    return boost::none;
  }

  ParamDomain param = ParamDomain::bottom();
  if (is_abstract(callee)) {
    always_assert(opcode == OPCODE_INVOKE_VIRTUAL ||
                  opcode == OPCODE_INVOKE_INTERFACE);
  } else {
    const auto& mwrpit = methods_which_return_parameter.find(callee);
    if (mwrpit == methods_which_return_parameter.end()) {
      return boost::none;
    }
    param = ParamDomain(mwrpit->second);
  }

  if (opcode == OPCODE_INVOKE_VIRTUAL || opcode == OPCODE_INVOKE_INTERFACE) {
    always_assert(callee->is_virtual());
    // Make sure all implementations of this method have the same param index
    const auto overriding_methods =
        method_override_graph::get_overriding_methods(m_graph, callee);
    for (auto* overriding : overriding_methods) {
      const auto& mwrpit = methods_which_return_parameter.find(overriding);
      if (mwrpit == methods_which_return_parameter.end()) {
        return boost::none;
      }
      param.join_with(ParamDomain(mwrpit->second));
      if (param.is_top()) {
        // Bail out early if possible; it's the common case
        return boost::none;
      }
    }

    // TODO: Are we doing something about abstract methods without any overrides
    // somewhere?
    always_assert(!param.is_bottom() || is_abstract(callee));
  }

  return param.get_constant();
}

bool ReturnParamResolver::returns_receiver(const DexMethodRef* method) const {
  // Hard-coded very special knowledge about certain framework methods

  // StringBuilder methods with result type StringBuilder return the receiver
  if (method->get_class() == m_string_builder_type &&
      method->get_proto()->get_rtype() == method->get_class()) {
    return true;
  }

  if (method == m_string_to_string_method) {
    return true;
  }

  return false;
}

const boost::optional<ParamIndex> ReturnParamResolver::get_return_param_index(
    cfg::ControlFlowGraph& cfg,
    const std::unordered_map<const DexMethod*, ParamIndex>&
        methods_which_return_parameter) const {
  Analyzer analyzer(cfg, *this, methods_which_return_parameter);
  auto return_param_index = ParamDomain::bottom();
  // join together return values of all blocks which end with a
  // return instruction
  for (const auto block : cfg.blocks()) {
    const auto& last = block->get_last_insn();
    if (last == block->end() || !is_return(last->insn->opcode())) {
      continue;
    }
    const auto env = analyzer.get_exit_state_at(block);
    const auto block_return_param_index = env.get(RETURN_VALUE);
    return_param_index.join_with(block_return_param_index);
  }

  return return_param_index.get_constant();
}

void ResultPropagation::patch(PassManager& mgr, IRCode* code) {
  // turn move-result-... into move instructions if the called method
  // is known to always return a particular parameter
  // TODO(T35815701): use cfg instead of code
  std::vector<IRInstruction*> deletes;
  const auto ii = InstructionIterable(code);
  for (auto it = ii.begin(); it != ii.end(); it++) {
    // do we have a sequence of invoke + move-result instruction?
    const auto insn = it->insn;
    if (!is_invoke(insn->opcode())) {
      continue;
    }
    const auto next = std::next(it);
    if (next == ii.end()) {
      continue;
    }
    const auto peek = next->insn;
    if (!is_move_result(peek->opcode())) {
      continue;
    }
    // do we know the invoked method always returns a particular parameter?
    const auto param_index = m_resolver.get_return_param_index(
        insn, m_methods_which_return_parameter, m_resolved_refs);
    if (!param_index) {
      continue;
    }

    if (!mgr.get_redex_options().verify_none_enabled) {
      // Let's check if replacing move-result with a move does not impact
      // verifiability.
      // TODO(configurability): Introduce a flag whether we care about
      // verifiability.
      // TODO(effectiveness): We are currently very consersative, only looking
      // locally at the proto's param type. Instead, track where the register
      // flowing into the invoke instruction was defined, and what its
      // statically known type is.
      const auto is_static = insn->opcode() == OPCODE_INVOKE_STATIC;
      const auto param_type =
          get_param_type(is_static, insn->get_method(), *param_index);
      const auto rtype = insn->get_method()->get_proto()->get_rtype();
      if (!check_cast(param_type, rtype)) {
        ++m_stats.unverifiable_move_results;
        continue;
      }
    }

    // rewrite instruction
    const auto source_reg = insn->src(*param_index);
    if (peek->dest() == source_reg) {
      deletes.push_back(peek);
      ++m_stats.erased_move_results;
    } else {
      patch_move_result_to_move(peek, source_reg);
      ++m_stats.patched_move_results;
    }
  }
  for (auto const instr : deletes) {
    code->remove_opcode(instr);
  }
}

void ResultPropagationPass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& /* conf */,
                                     PassManager& mgr) {
  const auto scope = build_class_scope(stores);
  const auto method_override_graph = method_override_graph::build_graph(scope);
  ReturnParamResolver resolver(*method_override_graph);
  const auto methods_which_return_parameter =
      find_methods_which_return_parameter(mgr, scope, resolver);

  const auto stats = walk::parallel::reduce_methods<ResultPropagation::Stats>(
      scope,
      [&](DexMethod* m) {
        const auto code = m->get_code();
        if (code == nullptr) {
          return ResultPropagation::Stats();
        }

        ResultPropagation rp(methods_which_return_parameter, resolver);
        rp.patch(mgr, code);
        return rp.get_stats();
      },
      [](ResultPropagation::Stats a, ResultPropagation::Stats b) {
        a.erased_move_results += b.erased_move_results;
        a.patched_move_results += b.patched_move_results;
        a.unverifiable_move_results += b.unverifiable_move_results;
        return a;
      });
  mgr.incr_metric(METRIC_METHODS_WHICH_RETURN_PARAMETER,
                  methods_which_return_parameter.size());
  mgr.incr_metric(METRIC_ERASED_MOVE_RESULTS, stats.erased_move_results);
  mgr.incr_metric(METRIC_PATCHED_MOVE_RESULTS, stats.patched_move_results);
  mgr.incr_metric(METRIC_UNVERIFIABLE_MOVE_RESULTS,
                  stats.unverifiable_move_results);
  TRACE(RP, 1,
        "result propagation --- potential methods: %d, erased moves: %d, "
        "patched moves: %d, "
        "unverifiable moves: %d\n",
        methods_which_return_parameter.size(), stats.erased_move_results,
        stats.patched_move_results, stats.unverifiable_move_results);
}

const std::unordered_map<const DexMethod*, ParamIndex>
ResultPropagationPass::find_methods_which_return_parameter(
    PassManager& mgr, const Scope& scope, const ReturnParamResolver& resolver) {
  walk::parallel::code(scope, [](DexMethod* method, IRCode& code) {
    const auto proto = method->get_proto();
    if (!proto->is_void()) {
      // void methods cannot return a parameter, skip expensive analysis
      code.build_cfg(/* editable */ true);
    }
  });

  std::unordered_map<const DexMethod*, ParamIndex>
      methods_which_return_parameter;
  // We iterate a few times to capture chains of method calls that all
  // eventually return `this`.
  // TODO(perf): Add flag to limit number of iterations
  // TODO(perf): For each analyzed method, keep track of the reasons (a set of
  // methods) why the call resolution gave up, and use that "dependency"
  // information to limit what needs to be processed in subsequent iterations
  while (true) {
    mgr.incr_metric(METRIC_METHODS_WHICH_RETURN_PARAMETER_ITERATIONS, 1);
    const auto next_methods_which_return_parameter =
        walk::parallel::reduce_methods<
            std::unordered_map<const DexMethod*, ParamIndex>>(
            scope,
            [&](DexMethod* method) {
              std::unordered_map<const DexMethod*, ParamIndex> res;

              const auto code = method->get_code();
              if (code == nullptr) {
                return res;
              }
              const auto proto = method->get_proto();
              if (proto->is_void()) {
                // void methods cannot return a parameter, skip expensive
                // analysis
                return res;
              }

              const auto mwrpit = methods_which_return_parameter.find(method);
              if (mwrpit != methods_which_return_parameter.end()) {
                // Short-circuit re-computing for perf
                res.insert({method, mwrpit->second});
                return res;
              }

              // TODO(T35815704): Make the cfg const
              cfg::ControlFlowGraph& cfg = code->cfg();
              const auto return_param_index = resolver.get_return_param_index(
                  cfg, methods_which_return_parameter);
              if (return_param_index) {
                res.insert({method, *return_param_index});
              }

              return res;
            },
            [](std::unordered_map<const DexMethod*, ParamIndex> a,
               std::unordered_map<const DexMethod*, ParamIndex> b) {
              a.insert(b.begin(), b.end());
              return a;
            });

    if (next_methods_which_return_parameter.size() ==
        methods_which_return_parameter.size()) {

      walk::parallel::code(scope, [](DexMethod* method, IRCode& code) {
        const auto proto = method->get_proto();
        if (!proto->is_void()) {
          code.clear_cfg();
        }
      });
      return methods_which_return_parameter;
    }
    methods_which_return_parameter = next_methods_which_return_parameter;
  }
}

static ResultPropagationPass s_pass;
