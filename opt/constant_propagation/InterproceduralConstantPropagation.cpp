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

#include "CallGraph.h"
#include "ConstantPropagation.h"
#include "ConstPropEnvironment.h"
#include "Timer.h"
#include "Walkers.h"

using namespace interprocedural_constant_propagation;

namespace {

/*
 * Describes the constant-valued arguments (if any) for a given method or
 * callsite. The n'th parameter will be represented by a binding of n to a
 * ConstantDomain instance.
 */
using ArgumentDomain = ConstPropEnvironment;

/*
 * Map of invoke-* instructions contained in some method M to their respective
 * ArgumentDomains. The ArgumentDomain at the entry to M (that is, the input
 * parameters to M) is bound to the null pointer.
 */
using Domain =
    PatriciaTreeMapAbstractEnvironment<const IRInstruction*, ArgumentDomain>;

constexpr IRInstruction* INPUT_ARGS = nullptr;

/*
 * Return an environment populated with parameter values.
 */
static ConstPropEnvironment env_with_params(const IRCode* code,
                                            const ArgumentDomain& args) {
  size_t idx{0};
  ConstPropEnvironment env;
  for (auto& mie : InstructionIterable(code->get_param_instructions())) {
    env.set(mie.insn->dest(), args.get(idx++));
  }
  return env;
}

/*
 * Performs intraprocedural constant propagation of stack / register values.
 */
class FixpointIterator
    : public MonotonicFixpointIterator<call_graph::GraphInterface, Domain> {
 public:
  FixpointIterator(const call_graph::Graph& call_graph,
                   const ConstPropConfig& config)
      : MonotonicFixpointIterator(call_graph), m_config(config) {}

  void analyze_node(DexMethod* const& method,
                    Domain* current_state) const override {
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

  Domain analyze_edge(const std::shared_ptr<call_graph::Edge>& edge,
                      const Domain& exit_state_at_source) const override {
    Domain entry_state_at_dest;
    auto it = edge->invoke_iterator();
    if (it == FatMethod::iterator()) {
      entry_state_at_dest.set(INPUT_ARGS, ConstPropEnvironment::top());
    } else {
      auto insn = it->insn;
      entry_state_at_dest.set(INPUT_ARGS, exit_state_at_source.get(insn));
    }
    return entry_state_at_dest;
  }

 private:
  ConstPropConfig m_config;
};

class Propagator {
 public:
  explicit Propagator(const Scope& scope, const ConstPropConfig& config)
      : m_scope(scope), m_config(config) {}

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
      } else if (args.is_value()) {
        TRACE(ICONSTP, 3, "Have args for %s: %s\n",
              SHOW(method), args.str().c_str());
      }
      IntraProcConstantPropagation intra_cp(code.cfg(), m_config);
      intra_cp.run(env_with_params(&code, args.get(INPUT_ARGS)));
      intra_cp.simplify();
      intra_cp.apply_changes(&code);
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
};

} // namespace

Stats InterproceduralConstantPropagationPass::run(Scope& scope) {
  Propagator propagator(scope, m_config);
  auto fp_iter = propagator.analyze(/* max_iterations */ 1);
  propagator.optimize(*fp_iter);
  return propagator.get_stats();
}

void InterproceduralConstantPropagationPass::run_pass(DexStoresVector& stores,
                                                      ConfigFiles&,
                                                      PassManager& mgr) {
  auto scope = build_class_scope(stores);
  const auto& stats = run(scope);
  mgr.incr_metric("constant_fields", stats.constant_fields);
  mgr.incr_metric("branches_removed", stats.branches_removed);
  mgr.incr_metric("materialized_consts", stats.materialized_consts);
}

static InterproceduralConstantPropagationPass s_pass;
