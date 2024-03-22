/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexUtil.h"
#include "HierarchyUtil.h"
#include "InitClassPruner.h"
#include "InitClassesWithSideEffects.h"
#include "LocalPointersAnalysis.h"
#include "MethodOverrideGraph.h"
#include "Purity.h"
#include "SideEffectSummary.h"
#include "SummarySerialization.h"

#include <unordered_map>

class ObjectSensitiveDce {
 public:
  struct Stats {
    std::unordered_map<uint16_t, size_t> invokes_with_summaries;
    init_classes::Stats init_class_stats;
    size_t external_escape_summaries{0};
    size_t external_side_effect_summaries{0};
    size_t removed_instructions{0};
    size_t init_class_instructions_added{0};
    size_t init_class_instructions_removed{0};
    size_t init_class_instructions_refined{0};
    size_t methods_with_summaries{0};
    size_t modified_params{0};
  };

  /*
   * This class tries to identify writes to registers and objects that never get
   * read from. Modeling dead object field writes is particularly useful in
   * conjunction with RemoveUnusedFieldsPass. Suppose we have an unused field
   * Foo.x:
   *
   *   new-instance v0 LFoo;
   *   invoke-direct {v0} LFoo;.<init>()V
   *   sput-object v0 LBar;.x:LFoo; # RMUF will remove this
   *
   * If we can determine that Foo's constructor does not modify anything
   * outside of its `this` argument, we will be able to remove the invoke-direct
   * call as well as the new-instance instruction.
   *
   * In contrast, LocalDce can only identify unused writes to registers -- it
   * knows nothing about objects. The trade-off is that this is takes much
   * longer to run.
   */

  explicit ObjectSensitiveDce(
      const Scope& scope,
      const init_classes::InitClassesWithSideEffects*
          init_classes_with_side_effects,
      const std::unordered_set<DexMethodRef*>& pure_methods,
      const method_override_graph::Graph& method_override_graph,
      const uint32_t big_override_threshold,
      local_pointers::SummaryMap* escape_summaries,
      side_effects::SummaryMap* effect_summaries)
      : m_scope(scope),
        m_init_classes_with_side_effects(init_classes_with_side_effects),
        m_pure_methods(pure_methods),
        m_method_override_graph(method_override_graph),
        m_big_override_threshold(big_override_threshold),
        m_escape_summaries(escape_summaries),
        m_effect_summaries(effect_summaries) {}

  const Stats& get_stats() const { return m_stats; }

  void dce();

 private:
  const Scope& m_scope;
  const init_classes::InitClassesWithSideEffects*
      m_init_classes_with_side_effects;
  const std::unordered_set<DexMethodRef*>& m_pure_methods;
  const method_override_graph::Graph& m_method_override_graph;
  uint32_t m_big_override_threshold;
  // The following are mutated internally.
  local_pointers::SummaryMap* m_escape_summaries;
  side_effects::SummaryMap* m_effect_summaries;
  Stats m_stats;
};
