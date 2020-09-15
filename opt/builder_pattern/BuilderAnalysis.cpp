// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#include "BuilderAnalysis.h"

#include "BaseIRAnalyzer.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "IRCode.h"
#include "Liveness.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "Resolver.h"
#include "Show.h"
#include "Trace.h"

using namespace sparta;

namespace builder_pattern {

namespace impl {

using namespace ir_analyzer;

using NullableConstantValue =
    acd_impl::ConstantAbstractValue<const IRInstruction*>;

class NullableConstantDomain final
    : public AbstractDomainScaffolding<NullableConstantValue,
                                       NullableConstantDomain> {
 public:
  using ConstantType = const IRInstruction*;
  using BaseClass =
      AbstractDomainScaffolding<NullableConstantValue, NullableConstantDomain>;

  NullableConstantDomain() { set_to_top(); }

  explicit NullableConstantDomain(const ConstantType& cst) {
    set_to_value(NullableConstantValue(cst));
  }

  explicit NullableConstantDomain(AbstractValueKind kind) : BaseClass(kind) {}

  boost::optional<ConstantType> get_constant() const {
    return is_value()
               ? boost::optional<ConstantType>(get_value()->get_constant())
               : boost::none;
  }

  void join_with(const NullableConstantDomain& other) override {
    if (is_value() && other.is_value()) {
      if (get_value()->get_constant()->opcode() == OPCODE_CONST &&
          other.get_value()->get_constant()->opcode() != OPCODE_CONST) {
        TRACE(BLD_PATTERN, 5, "Join NULL const with builder %s:%s",
              SHOW(get_value()->get_constant()),
              SHOW(other.get_value()->get_constant()));
        set_to_value(*other.get_value());
        return;
      }
      if (get_value()->get_constant()->opcode() != OPCODE_CONST &&
          other.get_value()->get_constant()->opcode() == OPCODE_CONST) {
        TRACE(BLD_PATTERN, 5, "Join NULL const with builder %s:%s",
              SHOW(get_value()->get_constant()),
              SHOW(other.get_value()->get_constant()));
        // Do nothing, the value is already builder.
        return;
      }
    }
    BaseClass::join_with(other);
  }
};

using IRInstructionConstantDomain = NullableConstantDomain;

/**
 * For each register that holds an instance of a builder keeps track
 * of the instruction that initialized it.
 **/
using IRInstructionConstantEnvironment =
    PatriciaTreeMapAbstractEnvironment<reg_t, IRInstructionConstantDomain>;

class InstructionToEnvMap final
    : public std::unordered_map<const IRInstruction*,
                                impl::IRInstructionConstantEnvironment> {};

class Analyzer final : public BaseIRAnalyzer<IRInstructionConstantEnvironment> {

 public:
  Analyzer(const cfg::ControlFlowGraph& cfg,
           const std::unordered_set<const DexType*>& types,
           const std::unordered_set<const DexType*>& exclude,
           bool accept_excluded)
      : BaseIRAnalyzer<IRInstructionConstantEnvironment>(cfg),
        m_types(types),
        m_exclude(exclude),
        m_accept_excluded(accept_excluded) {
    MonotonicFixpointIterator::run(IRInstructionConstantEnvironment::top());
  }

  void analyze_instruction(
      const IRInstruction* insn,
      IRInstructionConstantEnvironment* current_state) const override {

    auto default_case = [&]() {
      // If we get here, reset destination.
      if (insn->has_dest()) {
        current_state->set(insn->dest(), IRInstructionConstantDomain::top());
        if (insn->dest_is_wide()) {
          current_state->set(insn->dest() + 1,
                             IRInstructionConstantDomain::top());
        }
      } else if (insn->has_move_result_any()) {
        // Here we don't need to update RESULT_REGISTER + 1 for wide cases,
        // since we only care about keeping track of the builders
        // (which are not wide). We only use this result for object move result
        // opcodes.
        current_state->set(RESULT_REGISTER, IRInstructionConstantDomain::top());
      }
    };

    switch (insn->opcode()) {
    case OPCODE_MOVE_OBJECT: {
      current_state->set(insn->dest(), current_state->get(insn->src(0)));
      break;
    }

    case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    case OPCODE_MOVE_RESULT_OBJECT: {
      current_state->set(insn->dest(), current_state->get(RESULT_REGISTER));
      break;
    }

    case OPCODE_CONST:
      if (insn->get_literal() == 0) {
        // NULL const is a spcial case required for conditional creation.
        current_state->set(insn->dest(), IRInstructionConstantDomain(insn));
      } else {
        default_case();
      }
      break;

    case OPCODE_NEW_INSTANCE: {
      DexType* type = insn->get_type();
      if (m_types.count(type) > 0 &&
          (m_accept_excluded || m_exclude.count(type) == 0)) {
        // Keep track of the instantiation.
        current_state->set(RESULT_REGISTER, IRInstructionConstantDomain(insn));
      } else {
        default_case();
      }
      break;
    }

    case OPCODE_CHECK_CAST: {
      current_state->set(RESULT_REGISTER, current_state->get(insn->src(0)));
      break;
    }

    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_VIRTUAL:
    case OPCODE_INVOKE_STATIC: {
      auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
      if (!method) {
        default_case();
        break;
      }

      auto rtype = method->get_proto()->get_rtype();
      if (insn->opcode() != OPCODE_INVOKE_STATIC &&
          method->get_class() == rtype) {
        // NOTE: We expect that the method actually operates on the same
        //       instance and returns it. We are going to verify that later.
        current_state->set(RESULT_REGISTER, current_state->get(insn->src(0)));
      } else if (m_types.count(rtype) &&
                 (m_accept_excluded || m_exclude.count(rtype) == 0)) {
        // Keep track of the callsite that created / got the instance..
        current_state->set(RESULT_REGISTER, IRInstructionConstantDomain(insn));
      } else {
        default_case();
      }
      break;
    }

    default: {
      default_case();
      break;
    }
    }
  }

 private:
  const std::unordered_set<const DexType*>& m_types;
  const std::unordered_set<const DexType*>& m_exclude;
  bool m_accept_excluded;
};

} // namespace impl

BuilderAnalysis::~BuilderAnalysis() {}

BuilderAnalysis::BuilderAnalysis(
    const std::unordered_set<const DexType*>& types,
    const std::unordered_set<const DexType*>& exclude,
    DexMethod* method)
    : m_types(types),
      m_exclude(exclude),
      m_insn_to_env(new impl::InstructionToEnvMap),
      m_method(method),
      m_accept_excluded(true) {}

void BuilderAnalysis::run_analysis() {
  auto* code = m_method->get_code();
  if (!code) {
    return;
  }

  code->build_cfg(/* editable */ false);
  cfg::ControlFlowGraph& cfg = code->cfg();
  cfg.calculate_exit_block();
  m_analyzer.reset(
      new impl::Analyzer(cfg, m_types, m_exclude, m_accept_excluded));

  populate_usage();
  update_stats();
}

void BuilderAnalysis::print_usage() {
  if (!m_method || !m_method->get_code()) {
    return;
  }

  always_assert(m_analyzer);
  if (m_usage.empty()) {
    return;
  }

  TRACE(BLD_PATTERN, 4, "\nMethod %s", SHOW(m_method));
  for (const auto& pair : m_usage) {
    TRACE(BLD_PATTERN, 4, "\nInitialization in %s", SHOW(pair.first));

    for (const auto& insn : pair.second) {
      TRACE(BLD_PATTERN, 4, "\t Usage: %s", SHOW(insn));
    }
  }
}

void BuilderAnalysis::update_stats() {
  // We only keep track of total usages once per method, to avoid redundant
  // computation. At the same time, we switch m_accept_excluded to false,
  // which will ignore the excluded types in the analysis.
  //
  // TODO(emmasevastian): maybe move this to the caller instead?
  if (m_accept_excluded) {
    m_total_usages = m_usage.size() + m_excluded_instantiation.size();
    m_accept_excluded = false;
  }
}

namespace {

DexType* get_instantiated_type(const IRInstruction* insn) {
  DexType* current_instance = nullptr;

  switch (insn->opcode()) {
  case OPCODE_CONST:
    break;

  case OPCODE_NEW_INSTANCE:
    current_instance = insn->get_type();
    break;

  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_DIRECT: {
    auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
    current_instance = method->get_proto()->get_rtype();
    break;
  }

  default:
    not_reached_log("Different instantion opcode %s", SHOW(insn));
  }

  return current_instance;
}

} // namespace

void BuilderAnalysis::populate_usage() {
  m_usage.clear();
  m_insn_to_env->clear();

  auto* code = m_method->get_code();
  auto& cfg = code->cfg();

  // If the instantiated type is not excluded, updates the usages map.
  // Otherise, updating the excluded instantiation list.
  auto update_usages = [&](const IRInstruction* val, IRInstruction* insn) {
    if (auto referenced_type = get_instantiated_type(val)) {
      if (m_exclude.count(referenced_type) == 0) {
        m_usage[val].push_back(insn);
      }

      m_excluded_instantiation.emplace(val);
    }
  };

  for (cfg::Block* block : cfg.blocks()) {
    auto env = m_analyzer->get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      IRInstruction* insn = mie.insn;
      m_insn_to_env->emplace(insn, env);
      m_analyzer->analyze_instruction(insn, &env);

      if (insn->has_dest()) {
        auto dest = insn->dest();
        auto val_dest = env.get(dest).get_constant();
        if (val_dest) {
          update_usages(*val_dest, insn);
        }
      }

      for (size_t index = 0; index < insn->srcs_size(); index++) {
        auto src = insn->src(index);
        auto val_src = env.get(src).get_constant();
        if (val_src) {
          update_usages(*val_src, insn);
        }
      }
    }
  }
}

std::unordered_map<IRInstruction*, DexType*>
BuilderAnalysis::get_vinvokes_to_this_infered_type() {
  std::unordered_map<IRInstruction*, DexType*> result;

  for (const auto& pair : m_usage) {
    if (opcode::is_invoke_virtual(pair.first->opcode())) {
      always_assert(!result.count(const_cast<IRInstruction*>(pair.first)));

      auto current_instance = get_instantiated_type(pair.first);
      result[const_cast<IRInstruction*>(pair.first)] = current_instance;
    }

    for (auto& insn : pair.second) {
      if (opcode::is_invoke_virtual(insn->opcode())) {
        auto this_reg = insn->src(0);
        auto val = m_insn_to_env->at(insn).get(this_reg).get_constant();

        if (val) {
          auto infered_type = get_instantiated_type(*val);
          always_assert(!result.count(const_cast<IRInstruction*>(insn)) ||
                        result[const_cast<IRInstruction*>(insn)] ==
                            infered_type);
          result[const_cast<IRInstruction*>(insn)] = infered_type;
        };
      }
    }
  }

  return result;
}

std::unordered_set<IRInstruction*> BuilderAnalysis::get_all_inlinable_insns() {
  std::unordered_set<IRInstruction*> result;

  for (const auto& pair : m_usage) {
    if (opcode::is_an_invoke(pair.first->opcode())) {
      result.emplace(const_cast<IRInstruction*>(pair.first));
    }

    for (auto& insn : pair.second) {
      if (opcode::is_an_invoke(insn->opcode())) {
        result.emplace(const_cast<IRInstruction*>(insn));
      }
    }
  }

  // Filter out non-inlinable ones.
  for (auto it = result.begin(); it != result.end();) {
    auto insn = *it;
    always_assert(insn->has_method());

    auto method = resolve_method(insn->get_method(), opcode_to_search(insn));
    if (!method || !method->get_code()) {
      it = result.erase(it);
      continue;
    }

    if (method::is_init(method)) {
      auto this_reg = insn->src(0);
      auto val = m_insn_to_env->at(insn).get(this_reg).get_constant();
      if (!val || get_instantiated_type(*val) != method->get_class()) {
        it = result.erase(it);
        continue;
      }
    }

    it++;
  }

  return result;
}

std::unordered_set<const DexType*> BuilderAnalysis::get_instantiated_types(
    std::unordered_set<const IRInstruction*>* insns) {
  std::unordered_set<const DexType*> result;
  bool check_specific_insn = insns != nullptr;

  for (const auto& pair : m_usage) {
    auto type = get_instantiated_type(pair.first);

    if (!check_specific_insn) {
      result.emplace(type);
      continue;
    } else if (insns->count(const_cast<IRInstruction*>(pair.first))) {
      result.emplace(type);
      continue;
    }

    for (const auto& insn : pair.second) {
      if (insns->count(const_cast<IRInstruction*>(insn))) {
        result.emplace(type);
        break;
      }
    }
  }

  return result;
}

namespace {

DexMethodRef* get_obj_default_ctor() {
  auto obj_type = type::java_lang_Object();
  auto ctor_name = DexString::get_string("<init>");
  auto default_ctor = DexMethod::get_method(
      obj_type,
      ctor_name,
      DexProto::get_proto(type::_void(), DexTypeList::make_type_list({})));

  return default_ctor;
}

} // namespace

std::unordered_set<DexType*> BuilderAnalysis::non_removable_types() {
  auto non_removable_types = escape_types();

  // Consider other non-removable usages (for example synchronization usage).
  for (const auto& pair : m_usage) {
    auto current_instance = get_instantiated_type(pair.first);

    if (non_removable_types.count(current_instance)) {
      // Already decided it isn't removable.
      continue;
    }

    for (const IRInstruction* insn : pair.second) {
      if (opcode::is_a_monitor(insn->opcode())) {
        non_removable_types.emplace(current_instance);
        break;
      }
    }
  }

  return non_removable_types;
}

std::unordered_set<DexType*> BuilderAnalysis::escape_types() {
  auto* code = m_method->get_code();
  auto& cfg = code->cfg();

  // Don't treat as escaping a builder passed to Object.<init>().
  auto acceptable_method = get_obj_default_ctor();

  std::unordered_set<DexType*> escape_types;
  for (const auto& pair : m_usage) {
    auto instantiation_insn = pair.first;
    auto current_instance = get_instantiated_type(instantiation_insn);

    for (const auto& insn : pair.second) {
      // If there is any invoke here, it is because we couldn't inline it.
      if (opcode::is_an_invoke(insn->opcode())) {
        // We accept Object.<init> calls.
        if (insn->get_method() == acceptable_method) {
          continue;
        }

        auto method = resolve_method(insn->get_method(), MethodSearch::Any);

        TRACE(BLD_PATTERN, 2, "Excluding type %s since we couldn't inline %s",
              SHOW(current_instance), SHOW(method));
        escape_types.emplace(current_instance);
      } else if (insn->opcode() == OPCODE_INSTANCE_OF) {
        TRACE(BLD_PATTERN, 2, "Excluding type %s since instanceof used",
              SHOW(current_instance));
        escape_types.emplace(current_instance);
      } else if (opcode::is_an_iput(insn->opcode()) ||
                 opcode::is_an_sput(insn->opcode()) ||
                 insn->opcode() == OPCODE_APUT_OBJECT ||
                 opcode::is_a_return(insn->opcode())) {
        auto src = insn->src(0);

        auto escaped = m_insn_to_env->at(insn).get(src).get_constant();
        if (escaped && escaped.get() == instantiation_insn) {
          TRACE(BLD_PATTERN, 2,
                "Excluding type %s since it is stored or returned in %s",
                SHOW(current_instance), SHOW(insn));
          escape_types.emplace(current_instance);
        }
      }
    }
  }

  LivenessFixpointIterator liveness_iter(cfg);
  liveness_iter.run(LivenessDomain());

  for (cfg::Block* block : cfg.blocks()) {
    auto current_env = m_analyzer->get_exit_state_at(block);

    for (auto& succ : block->succs()) {
      cfg::Block* block_succ = succ->target();
      auto entry_env_at_succ = m_analyzer->get_entry_state_at(block_succ);
      auto live_in_vars_at_succ =
          liveness_iter.get_live_in_vars_at(succ->target());

      // Check that live registers, if they hold a builder at the end of block
      // B, then they hold the same one at the entry of any successor block.
      for (reg_t live_reg : live_in_vars_at_succ.elements()) {
        if (!entry_env_at_succ.get(live_reg).equals(
                current_env.get(live_reg))) {
          // It escapes here, since we couldn't follow it anymore.
          TRACE(BLD_PATTERN, 5,
                "Liveness missmatch for register v%d\nPRED:\n%sSUCC:\n%s",
                live_reg, SHOW(block), SHOW(block_succ));
          TRACE(BLD_PATTERN, 5, "Register value in PRED: %s",
                SHOW(*current_env.get(live_reg).get_constant()));
          if (auto succ_val = entry_env_at_succ.get(live_reg).get_constant()) {
            TRACE(BLD_PATTERN, 5, "Register value in SUCC: %s",
                  SHOW(*succ_val));
          } else {
            TRACE(BLD_PATTERN, 5, "Register value in SUCC: NONE");
          }

          auto init_insn = *current_env.get(live_reg).get_constant();
          if (init_insn->opcode() != OPCODE_CONST) {
            // NULL const cannot escape only builder can escape.
            auto current_instance = get_instantiated_type(init_insn);
            TRACE(BLD_PATTERN, 2,
                  "Excluding type %s since it escapes method %s",
                  SHOW(current_instance), SHOW(m_method));
            escape_types.emplace(current_instance);
          }
        }
      }
    }
  }

  return escape_types;
}

} // namespace builder_pattern
