/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <unordered_set>

#include <sparta/S_Expression.h>

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "InitClassesWithSideEffects.h"
#include "LocalPointersAnalysis.h"
#include "ReachingDefinitions.h"
#include "Resolver.h"

/*
 * This analysis identifies the side effects that methods have. A significant
 * portion of this is classifying heap mutations. We have three possible
 * categories:
 *
 *   1) Writes to locally-allocated non-escaping objects
 *   2) Writes to objects passed in as a parameter
 *   3) Writes to an escaping and/or unknown object
 *
 * Now supposing that there are no other side effects in the method (such as
 * throwing an exception), we can use this classification as follows:
 *
 *   - Methods containing only #1 are always pure and can be elided if their
 *     return values are unused.
 *   - Methods containing only #1 and #2 can be elided if their arguments are
 *     all non-escaping and unused, and if their return values are unused.
 */

using param_idx_t = uint16_t;

namespace side_effects {

using ParamInstructionMap =
    std::unordered_map<const IRInstruction*, param_idx_t>;

enum Effects : size_t {
  EFF_NONE = 0,
  EFF_THROWS = 1,
  EFF_LOCKS = 1 << 1,
  EFF_WRITE_MAY_ESCAPE = 1 << 2,
  EFF_UNKNOWN_INVOKE = 1 << 3,
  // Marked by @DoNotOptimize
  EFF_NO_OPTIMIZE = 1 << 4,
  EFF_INIT_CLASS = 1 << 5,
  EFF_NORMALIZED = 1 << 6,
};

struct Summary {
  // Currently, DCE only checks if a method has EFF_NONE -- otherwise it is
  // never removable. It doesn't dig into the specific reasons for the side
  // effects.
  size_t effects{EFF_NONE};
  std::unordered_set<param_idx_t> modified_params;
  bool may_read_external{false};

  Summary() = default;

  Summary(size_t effects,
          const std::initializer_list<param_idx_t>& modified_params,
          bool may_read_external = false)
      : effects(effects),
        modified_params(modified_params),
        may_read_external(may_read_external) {}

  Summary(const std::initializer_list<param_idx_t>& modified_params)
      : modified_params(modified_params) {}

  bool is_pure() const {
    return effects == EFF_NONE && modified_params.empty() && !may_read_external;
  }

  friend bool operator==(const Summary& a, const Summary& b) {
    return a.effects == b.effects && a.modified_params == b.modified_params &&
           a.may_read_external == b.may_read_external;
  }

  void normalize() {
    if (effects != EFF_NONE) {
      effects = EFF_NORMALIZED;
      modified_params.clear();
      may_read_external = false;
    }
  }

  static Summary from_s_expr(const sparta::s_expr&);
};

sparta::s_expr to_s_expr(const Summary&);

using SummaryMap = std::unordered_map<const DexMethodRef*, Summary>;

using InvokeToSummaryMap = std::unordered_map<const IRInstruction*, Summary>;

class SummaryBuilder final {
 public:
  explicit SummaryBuilder(const init_classes::InitClassesWithSideEffects&
                              init_classes_with_side_effects,
                          const InvokeToSummaryMap& invoke_to_summary_cmap,
                          const local_pointers::FixpointIterator& ptrs_fp_iter,
                          const IRCode* code,
                          reaching_defs::MoveAwareFixpointIterator*
                              reaching_defs_fixpoint_iter = nullptr,
                          bool analyze_external_reads = false);
  Summary build();

 private:
  void analyze_instruction_effects(
      const local_pointers::Environment& env,
      const reaching_defs::Environment& reaching_def_env,
      const IRInstruction* insn,
      Summary* summary);
  void classify_heap_write(const local_pointers::Environment& env,
                           reg_t modified_ptr_reg,
                           Summary* summary);
  // Map of load-param instruction -> parameter index
  ParamInstructionMap m_param_insn_map;
  const init_classes::InitClassesWithSideEffects&
      m_init_classes_with_side_effects;
  const InvokeToSummaryMap& m_invoke_to_summary_cmap;
  const local_pointers::FixpointIterator& m_ptrs_fp_iter;
  const IRCode* m_code;
  const bool m_analyze_external_reads;
  reaching_defs::MoveAwareFixpointIterator* m_reaching_defs_fixpoint_iter;
};

// Builds a caller-specific summary from.
InvokeToSummaryMap build_summary_map(const SummaryMap& summary_map,
                                     const call_graph::Graph& call_graph,
                                     const DexMethod* method);

// For testing.
Summary analyze_code(const init_classes::InitClassesWithSideEffects&
                         init_classes_with_side_effects,
                     const InvokeToSummaryMap& invoke_to_summary_cmap,
                     const local_pointers::FixpointIterator& ptrs_fp_iter,
                     const IRCode* code);

/*
 * Get the effect summary for all methods in scope.
 */
void analyze_scope(const init_classes::InitClassesWithSideEffects&
                       init_classes_with_side_effects,
                   const Scope& scope,
                   const call_graph::Graph&,
                   const local_pointers::FixpointIteratorMap&,
                   SummaryMap* effect_summaries);

} // namespace side_effects
