/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "InterproceduralConstantPropagation.h"

#include <mutex>

#include "ConstantEnvironment.h"
#include "ConstantPropagation.h"
#include "Timer.h"
#include "Walkers.h"

using namespace interprocedural_constant_propagation;
using namespace interprocedural_constant_propagation_impl;

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

} // namespace

namespace interprocedural_constant_propagation_impl {

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
  IntraProcConstantPropagation intra_cp(cfg, m_config);
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

} // interprocedural_constant_propagation_impl

namespace {

class Propagator {
 public:
  explicit Propagator(const Scope& scope,
                      const ConstPropConfig& config,
                      DexMethodRef* dynamic_check_fail_handler)
      : m_scope(scope),
        m_config(config),
        m_dynamic_check_fail_handler(dynamic_check_fail_handler) {}

  std::unique_ptr<FixpointIterator> analyze(size_t max_iterations) {
    call_graph::Graph cg(m_scope, m_config.include_virtuals);
    // Rebuild all CFGs here -- this should be more efficient than doing them
    // within FixpointIterator::analyze_node(), since that can get called
    // multiple times for a given method
    walk::parallel::code(m_scope, [](DexMethod*, IRCode& code) {
      code.build_cfg();
    });
    auto fp_iter = std::make_unique<FixpointIterator>(cg, m_config);
    fp_iter->run({{INPUT_ARGS, ArgumentDomain()}});
    return fp_iter;
  }

  /*
   * Run intraprocedural constant propagation on all methods, using the
   * information about constant method arguments that analyze() obtained.
   */
  void optimize(const FixpointIterator& fp_iter) {
    std::mutex stats_mutex;
    walk::parallel::code(m_scope, [&](DexMethod* method, IRCode& code) {
      auto args = fp_iter.get_entry_state_at(method);
      // If the callgraph isn't complete, reachable methods may appear
      // unreachable
      if (args.is_bottom()) {
        args.set_to_top();
      } else if (!args.is_top()) {
        TRACE(ICONSTP, 3, "Have args for %s: %s\n",
              SHOW(method), args.str().c_str());
      }
      IntraProcConstantPropagation intra_cp(code.cfg(), m_config);
      auto env = env_with_params(&code, args.get(INPUT_ARGS));
      intra_cp.run(env);
      intra_cp.simplify();
      intra_cp.apply_changes(&code);
      if (m_config.dynamic_input_checks) {
        insert_runtime_input_checks(env, m_dynamic_check_fail_handler, method);
      }
      {
        std::lock_guard<std::mutex> lock{stats_mutex};
        m_stats.branches_removed += intra_cp.branches_removed();
        m_stats.materialized_consts += intra_cp.materialized_consts();
      }
    });
  }

  const Stats& get_stats() const {
    return m_stats;
  }

 private:
  Stats m_stats;
  Scope m_scope;
  ConstPropConfig m_config;
  DexMethodRef* m_dynamic_check_fail_handler;
};

} // namespace

Stats InterproceduralConstantPropagationPass::run(Scope& scope) {
  Propagator propagator(scope, m_config, m_dynamic_check_fail_handler);
  auto fp_iter = propagator.analyze(/* max_iterations */ 1);
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
  mgr.incr_metric("constant_fields", stats.constant_fields);
  mgr.incr_metric("branches_removed", stats.branches_removed);
  mgr.incr_metric("materialized_consts", stats.materialized_consts);
}

static InterproceduralConstantPropagationPass s_pass;
