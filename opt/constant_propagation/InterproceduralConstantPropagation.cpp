/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "InterproceduralConstantPropagation.h"

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "Timer.h"
#include "Walkers.h"

using namespace constant_propagation;
using namespace constant_propagation::interprocedural;

namespace {

/*
 * Return an environment populated with parameter values.
 */
static ConstantEnvironment env_with_params(const IRCode* code,
                                           const ArgumentDomain& args) {
  size_t idx{0};
  ConstantEnvironment env;
  for (auto& mie : InstructionIterable(code->get_param_instructions())) {
    env.set(mie.insn->dest(), args.get(idx++));
  }
  return env;
}

/*
 * Initialize field_env with the encoded values of primitive fields. If no
 * encoded value is present, initialize them with a zero value (which is
 * the same thing that the runtime does).
 */
void set_fields_with_encoded_values(const Scope& scope,
                                    ConstantStaticFieldEnvironment* field_env) {
  for (const auto* cls : scope) {
    for (auto* sfield : cls->get_sfields()) {
      auto type = sfield->get_type();
      if (!is_primitive(type)) {
        continue;
      }
      auto value = sfield->get_static_value();
      if (value == nullptr) {
        field_env->set(sfield, SignedConstantDomain(0));
      } else {
        always_assert(value->is_evtype_primitive());
        field_env->set(sfield, SignedConstantDomain(value->value()));
      }
    }
  }
}

/*
 * Replace all sgets from constant fields with const opcodes, and delete all
 * sputs to those fields.
 */
void simplify_constant_fields(const Scope& scope,
                              const ConstantStaticFieldEnvironment& field_env) {
  return walk::parallel::methods(scope, [&](DexMethod* method) {
    IRCode* code = method->get_code();
    std::unordered_map<const IRInstruction*, IRInstruction*> replacements;
    std::vector<FatMethod::iterator> deletes;
    if (code == nullptr) {
      return;
    }
    for (auto& mie : InstructionIterable(code)) {
      auto* insn = mie.insn;
      auto op = insn->opcode();
      if (!insn->has_field()) {
        continue;
      }
      auto* field = resolve_field(insn->get_field());
      if (!field_env.get(field).constant_domain().is_value()) {
        continue;
      }
      TRACE(ICONSTP,
            3,
            "%s has value %s\n",
            SHOW(field),
            field_env.get(field).constant_domain().str().c_str());
      if (is_sget(op)) {
        IRInstruction* replacement{nullptr};
        if (op == OPCODE_SGET_WIDE) {
          replacement = new IRInstruction(OPCODE_CONST_WIDE);
        } else {
          replacement = new IRInstruction(OPCODE_CONST);
        }
        replacement->set_literal(
            *field_env.get(field).constant_domain().get_constant());
        replacement->set_dest(std::next(code->iterator_to(mie))->insn->dest());
        replacements.emplace(insn, replacement);
      } else if (is_sput(op)) {
        TRACE(ICONSTP, 3, "Found deletable sput in %s\n", SHOW(method));
        deletes.push_back(code->iterator_to(mie));
      }
    }
    for (auto& pair : replacements) {
      code->replace_opcode(const_cast<IRInstruction*>(pair.first), pair.second);
    }
    for (auto it : deletes) {
      code->remove_opcode(it);
    }
  });
}

} // namespace

namespace constant_propagation {

namespace interprocedural {

void FixpointIterator::analyze_node(DexMethod* const& method,
                                    Domain* current_state) const {
  // The entry node has no associated method.
  if (method == nullptr) {
    return;
  }
  auto code = method->get_code();
  if (code == nullptr) {
    return;
  }
  auto& cfg = code->cfg();
  intraprocedural::FixpointIterator intra_cp(cfg, m_config, m_field_env);
  intra_cp.run(env_with_params(code, current_state->get(INPUT_ARGS)));

  for (auto* block : cfg.blocks()) {
    auto state = intra_cp.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (is_invoke(insn->opcode())) {
        ArgumentDomain out_args;
        for (size_t i = 0; i < insn->srcs_size(); ++i) {
          out_args.set(i, state.get(insn->src(i)));
        }
        current_state->set(insn, out_args);
      }
      intra_cp.analyze_instruction(insn, &state);
    }
  }
}

Domain FixpointIterator::analyze_edge(
    const std::shared_ptr<call_graph::Edge>& edge,
    const Domain& exit_state_at_source) const {
  Domain entry_state_at_dest;
  auto it = edge->invoke_iterator();
  if (it == FatMethod::iterator()) {
    entry_state_at_dest.set(INPUT_ARGS, ConstantEnvironment::top());
  } else {
    auto insn = it->insn;
    entry_state_at_dest.set(INPUT_ARGS, exit_state_at_source.get(insn));
  }
  return entry_state_at_dest;
}

static IROpcode opcode_for_interval(const sign_domain::Interval intv) {
  using namespace sign_domain;
  switch (intv) {
  case Interval::ALL:
  case Interval::EMPTY:
    always_assert_log(false, "Cannot generate opcode for this interval");
    not_reached();
  case Interval::SIZE:
    not_reached();
  case Interval::LTZ:
    return OPCODE_IF_LTZ;
  case Interval::GTZ:
    return OPCODE_IF_GTZ;
  case Interval::EQZ:
    return OPCODE_IF_EQZ;
  case Interval::GEZ:
    return OPCODE_IF_GEZ;
  case Interval::LEZ:
    return OPCODE_IF_LEZ;
  }
}

/*
 * Insert code at the start of the method that checks that the arguments that
 * our static analysis thinks are constant actually have those values at
 * runtime. If the check fails, the code will call out to
 * dynamic_check_fail_handler, passing it a single integer that indicates the
 * index of the failing parameter.
 */
void insert_runtime_input_checks(const ConstantEnvironment& env,
                                 DexMethodRef* dynamic_check_fail_handler,
                                 DexMethod* method) {
  if (!env.is_value()) {
    return;
  }
  auto arg_types = method->get_proto()->get_args()->get_type_list();
  auto* code = method->get_code();
  auto param_insns = code->get_param_instructions();
  auto insert_it = param_insns.end();
  auto insn_it = InstructionIterable(code).begin();
  if (!is_static(method)) {
    // Skip the load-param instruction for the `this` argument
    ++insn_it;
  }
  // We do not want to iterate over InstructionIterable(param_insns) here
  // because we are inserting MIEs that will move the end iterator of
  // param_insns
  for (uint32_t i = 0; i < arg_types.size(); ++i, ++insn_it) {
    auto* arg_type = arg_types.at(i);
    // We don't currently support floating-point or long types...
    if (!(is_integer(arg_type) || is_object(arg_type))) {
      continue;
    }
    auto reg = insn_it->insn->dest();
    auto scd = env.get(reg);
    if (scd.is_top()) {
      continue;
    }
    // The branching instruction that checks whether the constant domain is
    // correct for the given param
    FatMethod::iterator check_insn_it;
    const auto& cst = scd.constant_domain().get_constant();
    if (cst) {
      // If we have an exact constant, create a const instruction that loads
      // that value and check for equality.
      auto cst_reg = code->allocate_temp();
      code->insert_before(insert_it,
                          (new IRInstruction(OPCODE_CONST))
                              ->set_dest(cst_reg)
                              ->set_literal(*cst));
      check_insn_it = code->insert_before(insert_it,
                                          (new IRInstruction(OPCODE_IF_EQ))
                                              ->set_src(0, reg)
                                              ->set_src(1, cst_reg));
    } else {
      // We don't have a constant, but we have a range. Insert the appropriate
      // if-* instruction that checks that the argument falls in the range.
      check_insn_it = code->insert_before(
          insert_it,
          (new IRInstruction(opcode_for_interval(scd.interval())))
              ->set_src(0, reg));
    }
    // If the branch in check_insn_it does not get taken, it means the
    // check failed. So we call the error handler here.
    auto tmp = code->allocate_temp();
    code->insert_before(
        insert_it,
        ((new IRInstruction(OPCODE_CONST))->set_dest(tmp)->set_literal(i)));
    code->insert_before(insert_it,
                        ((new IRInstruction(OPCODE_INVOKE_STATIC))
                             ->set_method(dynamic_check_fail_handler)
                             ->set_arg_word_count(1)
                             ->set_src(0, tmp)));
    auto bt = new BranchTarget(&*check_insn_it);
    code->insert_before(insert_it, bt);
  }
}

} // namespace interprocedural

} // namespace constant_propagation

namespace {

class Propagator {
 public:
  explicit Propagator(const Scope& scope,
                      const ConstPropConfig& config,
                      DexMethodRef* dynamic_check_fail_handler)
      : m_scope(scope),
        m_config(config),
        m_dynamic_check_fail_handler(dynamic_check_fail_handler) {}

  /*
   * We start off by assuming no knowledge of any field values, i.e. we just
   * interprocedurally propagate constants from const opcodes. Next, we look
   * at every sput instruction. If they all write the same value to a given
   * field, then we record in the field environment that the field is constant.
   * If any such fields were found, we repeat the propagation step.
   */
  std::unique_ptr<FixpointIterator> analyze() {
    call_graph::Graph cg(m_scope, m_config.include_virtuals);
    // Rebuild all CFGs here -- this should be more efficient than doing them
    // within FixpointIterator::analyze_node(), since that can get called
    // multiple times for a given method
    walk::parallel::code(m_scope,
                         [](DexMethod*, IRCode& code) { code.build_cfg(); });
    auto fp_iter = std::make_unique<FixpointIterator>(cg, m_config);
    fp_iter->run({{INPUT_ARGS, ArgumentDomain()}});

    ConstantStaticFieldEnvironment field_env;
    set_fields_with_encoded_values(m_scope, &field_env);
    for (size_t i = 0; i < m_config.max_heap_analysis_iterations; ++i) {
      join_all_field_values(*fp_iter, &field_env);
      if (field_env.equals(fp_iter->get_field_environment())) {
        break;
      }
      fp_iter->set_field_environment(field_env);
      fp_iter->run({{INPUT_ARGS, ArgumentDomain()}});
    }

    m_stats.constant_fields = field_env.is_value() ? field_env.size() : 0;
    return fp_iter;
  }

  /*
   * Run intraprocedural constant propagation on all methods, using the
   * information about constant method arguments that analyze() obtained.
   */
  void optimize(const FixpointIterator& fp_iter) {
    using Data = std::nullptr_t;
    m_stats.transform_stats =
        walk::parallel::reduce_methods<Data, Transform::Stats>(
            m_scope,
            [&](Data&, DexMethod* method) {
              if (method->get_code() == nullptr) {
                return Transform::Stats();
              }
              auto& code = *method->get_code();
              auto args = fp_iter.get_entry_state_at(method);
              // If the callgraph isn't complete, reachable methods may appear
              // unreachable
              if (args.is_bottom()) {
                args.set_to_top();
              } else if (!args.is_top()) {
                TRACE(ICONSTP,
                      3,
                      "Have args for %s: %s\n",
                      SHOW(method),
                      args.str().c_str());
              }

              intraprocedural::FixpointIterator intra_cp(
                  code.cfg(), m_config, fp_iter.get_field_environment());
              auto env = env_with_params(&code, args.get(INPUT_ARGS));
              intra_cp.run(env);
              Transform tf(m_config);
              auto stats = tf.apply(intra_cp, &code);

              if (m_config.dynamic_input_checks) {
                insert_runtime_input_checks(
                    env, m_dynamic_check_fail_handler, method);
              }

              return stats;
            },
            [](Transform::Stats a, Transform::Stats b) { // reducer
              return a + b;
            },
            [&](unsigned int) { // data initializer
              return nullptr;
            });
    simplify_constant_fields(m_scope, fp_iter.get_field_environment());
  }

  /*
   * For each static field, join all the values that may have been written to
   * it at any point in the program.
   *
   * XXX the only reason this method is part of Propagator is because of the
   * need to share ConstPropConfig... we should look at factoring it out
   */
  void join_all_field_values(const FixpointIterator& fp_iter,
                             ConstantStaticFieldEnvironment* field_env) {
    walk::methods(m_scope, [&](DexMethod* method) {
      IRCode* code = method->get_code();
      if (code == nullptr) {
        return;
      }
      auto& cfg = code->cfg();
      auto args = fp_iter.get_entry_state_at(method);
      // If the callgraph isn't complete, reachable methods may appear
      // unreachable
      if (args.is_bottom()) {
        args.set_to_top();
      }
      intraprocedural::FixpointIterator intra_cp(code->cfg(), m_config);
      intra_cp.run(env_with_params(code, args.get(nullptr)));
      for (Block* b : cfg.blocks()) {
        auto state = intra_cp.get_entry_state_at(b);
        for (auto& mie : InstructionIterable(b)) {
          auto* insn = mie.insn;
          auto op = insn->opcode();
          if (is_sput(op)) {
            auto value = state.get(insn->src(0));
            auto field = resolve_field(insn->get_field());
            if (field != nullptr) {
              field_env->update(field, [&value](auto current_value) {
                return current_value.join(value);
              });
            }
          }
          intra_cp.analyze_instruction(insn, &state);
        }
      }
    });
  }

  const Stats& get_stats() const { return m_stats; }

 private:
  Stats m_stats;
  Scope m_scope;
  ConstPropConfig m_config;
  DexMethodRef* m_dynamic_check_fail_handler;
};

} // namespace

Stats InterproceduralConstantPropagationPass::run(Scope& scope) {
  Propagator propagator(scope, m_config, m_dynamic_check_fail_handler);
  auto fp_iter = propagator.analyze();
  propagator.optimize(*fp_iter);
  return propagator.get_stats();
}

void InterproceduralConstantPropagationPass::run_pass(DexStoresVector& stores,
                                                      ConfigFiles& config,
                                                      PassManager& mgr) {
  if (m_config.dynamic_input_checks) {
    auto& pg_map = config.get_proguard_map();
    m_dynamic_check_fail_handler =
        DexMethod::get_method(pg_map.translate_method(
            "Lcom/facebook/redex/ConstantPropagationAssertHandler;.fail:(I)V"));
    always_assert_log(m_dynamic_check_fail_handler &&
                          m_dynamic_check_fail_handler->is_def(),
                      "Could not find dynamic check handler");
  }

  auto scope = build_class_scope(stores);
  const auto& stats = run(scope);
  mgr.incr_metric("branches_removed", stats.transform_stats.branches_removed);
  mgr.incr_metric("materialized_consts",
                  stats.transform_stats.materialized_consts);
  mgr.incr_metric("constant_fields", stats.constant_fields);
}

static InterproceduralConstantPropagationPass s_pass;
