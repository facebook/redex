/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <utility>

#include "ConstantPropagationRuntimeAssert.h"
#include "ConstantPropagationTransform.h"
#include "ConstantPropagationWholeProgramState.h"
#include "IPConstantPropagationAnalysis.h"
#include "Pass.h"

namespace constant_propagation {

namespace interprocedural {

class PassImpl : public Pass {
 public:
  struct Config {
    bool include_virtuals{false};
    bool use_multiple_callee_callgraph{false};
    bool create_runtime_asserts{false};
    // The maximum number of times we will try to refine the WholeProgramState.
    // Setting this to zero means that all field values and return values will
    // be treated as Top.
    uint64_t max_heap_analysis_iterations{0};
    uint32_t big_override_threshold{5};
    std::unordered_set<const DexType*> field_blocklist;
    bool compute_definitely_assigned_ifields{true};

    Transform::Config transform;
    RuntimeAssertTransform::Config runtime_assert;
  };

  redex_properties::PropertyInteractions get_property_interactions()
      const override {
    using namespace redex_properties::names;
    return {{HasSourceBlocks, {.preserves = true}},
            {NoSpuriousGetClassCalls, {.preserves = true}}};
  }

  explicit PassImpl(Config config)
      : Pass("InterproceduralConstantPropagationPass"),
        m_config(std::move(config)) {}

  PassImpl() : PassImpl(Config()) {}

  void bind_config() override {
    bind("replace_moves_with_consts",
         true,
         m_config.transform.replace_moves_with_consts);
    bind("remove_dead_switch", true, m_config.transform.remove_dead_switch);
    bind("include_virtuals", false, m_config.include_virtuals);
    bind("use_multiple_callee_callgraph",
         false,
         m_config.use_multiple_callee_callgraph);
    bind("big_override_threshold", UINT32_C(5),
         m_config.big_override_threshold);
    bind("create_runtime_asserts", false, m_config.create_runtime_asserts);
    bind("max_heap_analysis_iterations",
         UINT64_C(0),
         m_config.max_heap_analysis_iterations);
    bind("field_blocklist",
         {},
         m_config.field_blocklist,
         "List of types whose fields that this optimization will omit.");
    bind("compute_definitely_assigned_ifields", true,
         m_config.compute_definitely_assigned_ifields,
         "Whether to predict which instance fields are always written before "
         "they are read, in order to ignore the default value 0.");
  }

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;

  /*
   * run_pass() takes a PassManager object, making it awkward to call in unit
   * tests. run() is a more direct way to call this pass. The caller is
   * responsible for picking the right Config settings.
   */
  void run(const DexStoresVector& stores, int min_sdk = 0);

  /*
   * Exposed for testing purposes.
   */
  std::unique_ptr<FixpointIterator> analyze(
      const Scope&,
      const ImmutableAttributeAnalyzerState*,
      const ApiLevelAnalyzerState* api_level_analyzer_state);

 private:
  void compute_analysis_stats(const WholeProgramState&,
                              const std::unordered_set<const DexField*>&);

  void optimize(const Scope&,
                const XStoreRefs& xstores,
                const FixpointIterator&,
                const ImmutableAttributeAnalyzerState*);

  struct Stats {
    // Number of instance fields that are known to be definitely-assigned, i.e.
    // they are being written to before read during their object's construction.
    size_t definitely_assigned_ifields{0};
    // Number of definitely-assigned instance fields for which useful constant
    // values were found; a "useful constant value" is one that is not top, or
    // in case of Booleans 0 or 1, but some other abstract ConstantValue.
    size_t constant_definitely_assigned_ifields{0};
    // Number of fields for which useful constant values were found.
    size_t constant_fields{0};
    // number of methods for which useful constant return values were found.
    size_t constant_methods{0};

    size_t callgraph_nodes{0};
    size_t callgraph_edges{0};
    size_t callgraph_callsites{0};

    FixpointIterator::Stats fp_iter;
  } m_stats;
  Transform::Stats m_transform_stats;
  Config m_config;
};

} // namespace interprocedural

} // namespace constant_propagation

using InterproceduralConstantPropagationPass =
    constant_propagation::interprocedural::PassImpl;
