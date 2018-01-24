/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <atomic>

#include "CallGraph.h"
#include "ConstPropConfig.h"
#include "ConstantEnvironment.h"
#include "Pass.h"

namespace interprocedural_constant_propagation {

struct Stats {
  size_t constant_fields{0};
  size_t branches_removed{0};
  size_t materialized_consts{0};
};

} // namespace interprocedural_constant_propagation

namespace interprocedural_constant_propagation_impl {

/*
 * Describes the constant-valued arguments (if any) for a given method or
 * callsite. The n'th parameter will be represented by a binding of n to a
 * ConstantDomain instance.
 */
using ArgumentDomain = ConstantEnvironment;

/*
 * Map of invoke-* instructions contained in some method M to their respective
 * ArgumentDomains. The ArgumentDomain at the entry to M (that is, the input
 * parameters to M) is bound to the null pointer.
 */
using Domain =
    PatriciaTreeMapAbstractEnvironment<const IRInstruction*, ArgumentDomain>;

constexpr IRInstruction* INPUT_ARGS = nullptr;

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
                    Domain* current_state) const override;

  Domain analyze_edge(const std::shared_ptr<call_graph::Edge>& edge,
                      const Domain& exit_state_at_source) const override;

 private:
  ConstPropConfig m_config;
};

void insert_runtime_input_checks(const ConstantEnvironment&,
                                 DexMethodRef*,
                                 DexMethod*);

} // namespace interprocedural_constant_propagation_impl

class InterproceduralConstantPropagationPass : public Pass {
 public:
  InterproceduralConstantPropagationPass()
      : Pass("InterproceduralConstantPropagationPass") {}

  void configure_pass(const PassConfig& pc) override {
    pc.get(
        "replace_moves_with_consts", false, m_config.replace_moves_with_consts);
    pc.get("fold_arithmetic", false, m_config.fold_arithmetic);
    pc.get("propagate_conditions", false, m_config.propagate_conditions);
    pc.get("include_virtuals", false, m_config.include_virtuals);
    pc.get("dynamic_input_checks", false, m_config.dynamic_input_checks);
  }

  // run() is exposed for testing purposes -- run_pass takes a PassManager
  // object, making it awkward to call in unit tests.
  interprocedural_constant_propagation::Stats run(Scope&);

  void run_pass(DexStoresVector& stores,
                ConfigFiles& cfg,
                PassManager& mgr) override;
 private:
  ConstPropConfig m_config;
  DexMethodRef* m_dynamic_check_fail_handler;
};
