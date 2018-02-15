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
#include "ConstantPropagationTransform.h"
#include "HashedAbstractPartition.h"
#include "Pass.h"

namespace constant_propagation {

struct Stats {
  Transform::Stats transform_stats;
  size_t constant_fields{0};
};

namespace interprocedural {

/*
 * Describes the constant-valued arguments (if any) for a given method or
 * callsite. The n'th argument will be represented by a binding of n to a
 * ConstantDomain instance.
 */
using ArgumentDomain = ConstantEnvironment;

/*
 * This map is an abstraction of the execution paths starting from the entry
 * point of a method and ending at an invoke instruction, hence the use of an
 * abstract partitioning.
 *
 * At method entry, this map contains a single item, a binding of the null
 * pointer to an ArgumentDomain representing the input arguments. At method
 * exit, this map will have bindings from all the invoke-* instructions
 * contained in the method to the ArgumentDomains representing the arguments
 * passed to the callee.
 */
using Domain = HashedAbstractPartition<const IRInstruction*, ArgumentDomain>;

constexpr IRInstruction* INPUT_ARGS = nullptr;

/*
 * Performs interprocedural constant propagation of stack / register values.
 */
class FixpointIterator
    : public MonotonicFixpointIterator<call_graph::GraphInterface, Domain> {
 public:
  FixpointIterator(const call_graph::Graph& call_graph,
                   const ConstPropConfig& config)
      : MonotonicFixpointIterator(call_graph),
        m_config(config) {}

  void analyze_node(DexMethod* const& method,
                    Domain* current_state) const override;

  Domain analyze_edge(const std::shared_ptr<call_graph::Edge>& edge,
                      const Domain& exit_state_at_source) const override;

  ConstantStaticFieldEnvironment get_field_environment() const {
    return m_field_env;
  }

  void set_field_environment(ConstantStaticFieldEnvironment env) {
    m_field_env = env;
  }

 private:
  ConstPropConfig m_config;
  ConstantStaticFieldEnvironment m_field_env;
};

void insert_runtime_input_checks(const ConstantEnvironment&,
                                 DexMethodRef*,
                                 DexMethod*);

} // namespace interprocedural

} // namespace constant_propagation

class InterproceduralConstantPropagationPass : public Pass {
 public:
  InterproceduralConstantPropagationPass(const ConstPropConfig& config)
      : Pass("InterproceduralConstantPropagationPass"), m_config(config) {}

  InterproceduralConstantPropagationPass()
      : InterproceduralConstantPropagationPass(ConstPropConfig()) {}

  void configure_pass(const PassConfig& pc) override {
    pc.get(
        "replace_moves_with_consts", false, m_config.replace_moves_with_consts);
    pc.get("fold_arithmetic", false, m_config.fold_arithmetic);
    pc.get("include_virtuals", false, m_config.include_virtuals);
    pc.get("dynamic_input_checks", false, m_config.dynamic_input_checks);
    int64_t max_heap_analysis_iterations;
    pc.get("max_heap_analysis_iterations", 0, max_heap_analysis_iterations);
    always_assert(max_heap_analysis_iterations >= 0);
    m_config.max_heap_analysis_iterations =
        static_cast<size_t>(max_heap_analysis_iterations);
  }

  // run() is exposed for testing purposes -- run_pass takes a PassManager
  // object, making it awkward to call in unit tests.
  constant_propagation::Stats run(Scope&);

  void run_pass(DexStoresVector& stores,
                ConfigFiles& cfg,
                PassManager& mgr) override;

 private:
  ConstPropConfig m_config;
  DexMethodRef* m_dynamic_check_fail_handler;
};
