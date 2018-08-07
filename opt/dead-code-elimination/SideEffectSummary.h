/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <ostream>
#include <unordered_map>
#include <unordered_set>

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "LocalPointersAnalysis.h"
#include "Resolver.h"
#include "S_Expression.h"

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

enum Effects : size_t {
  EFF_NONE = 0,
  EFF_THROWS = 1,
  EFF_LOCKS = 1 << 1,
  EFF_WRITE_MAY_ESCAPE = 1 << 2,
  EFF_UNKNOWN_INVOKE = 1 << 3,
};

struct Summary {
  // Currently, DCE only checks if a method has EFF_NONE -- otherwise it is
  // never removable. It doesn't dig into the specific reasons for the side
  // effects.
  size_t effects{EFF_NONE};
  std::unordered_set<param_idx_t> modified_params;

  Summary() = default;

  Summary(size_t effects,
          const std::initializer_list<param_idx_t>& modified_params)
      : effects(effects), modified_params(modified_params) {}

  Summary(const std::initializer_list<param_idx_t>& modified_params)
      : modified_params(modified_params) {}

  friend bool operator==(const Summary& a, const Summary& b) {
    return a.effects == b.effects && a.modified_params == b.modified_params;
  }

  static Summary from_s_expr(const sparta::s_expr&);
};

sparta::s_expr to_s_expr(const Summary&);

using SummaryMap = std::unordered_map<const DexMethodRef*, Summary>;

using InvokeToSummaryMap = std::unordered_map<const IRInstruction*, Summary>;

// For testing.
Summary analyze_code(const InvokeToSummaryMap& invoke_to_summary_cmap,
                     const local_pointers::FixpointIterator& ptrs_fp_iter,
                     const IRCode* code);

/*
 * Get the effect summary for all methods in scope.
 */
void analyze_scope(
    const Scope& scope,
    const call_graph::Graph&,
    ConcurrentMap<const DexMethodRef*, local_pointers::FixpointIterator*>&,
    SummaryMap* effect_summaries);

} // namespace side_effects
