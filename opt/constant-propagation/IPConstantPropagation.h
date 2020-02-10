/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <utility>

#include "ConstantPropagationRuntimeAssert.h"
#include "ConstantPropagationTransform.h"
#include "ConstantPropagationWholeProgramState.h"
#include "Pass.h"

namespace constant_propagation {

namespace interprocedural {

class PassImpl : public Pass {
 public:
  struct Config {
    bool include_virtuals{false};
    bool create_runtime_asserts{false};
    // The maximum number of times we will try to refine the WholeProgramState.
    // Setting this to zero means that all field values and return values will
    // be treated as Top.
    uint64_t max_heap_analysis_iterations{0};
    std::unordered_set<const DexType*> field_black_list;

    Transform::Config transform;
    RuntimeAssertTransform::Config runtime_assert;
  };

  PassImpl(Config config)
      : Pass("InterproceduralConstantPropagationPass"),
        m_config(std::move(config)) {}

  PassImpl() : PassImpl(Config()) {}

  void bind_config() override {
    bind("replace_moves_with_consts",
         true,
         m_config.transform.replace_moves_with_consts);
    bind("remove_dead_switch", true, m_config.transform.remove_dead_switch);
    bind("include_virtuals", false, m_config.include_virtuals);
    bind("create_runtime_asserts", false, m_config.create_runtime_asserts);
    bind("max_heap_analysis_iterations",
         UINT64_C(0),
         m_config.max_heap_analysis_iterations);
    bind("field_black_list",
         {},
         m_config.field_black_list,
         "List of types whose fields that this optimization will omit.");
  }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

  /*
   * run_pass() takes a PassManager object, making it awkward to call in unit
   * tests. run() is a more direct way to call this pass. The caller is
   * responsible for picking the right Config settings.
   */
  void run(Scope&);

  /*
   * Exposed for testing purposes.
   */
  std::unique_ptr<FixpointIterator> analyze(const Scope&);

 private:
  void compute_analysis_stats(const WholeProgramState&);

  void optimize(const Scope&, const FixpointIterator&);

  struct Stats {
    size_t constant_fields{0};
    size_t constant_methods{0};
  } m_stats;
  Transform::Stats m_transform_stats;
  Config m_config;
};

} // namespace interprocedural

} // namespace constant_propagation

using InterproceduralConstantPropagationPass =
    constant_propagation::interprocedural::PassImpl;
