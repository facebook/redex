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

enum Effects : size_t {
  EFF_NONE = 0,
  EFF_THROWS = 1,
  EFF_LOCKS = 1 << 1,
  EFF_WRITE_MAY_ESCAPE = 1 << 2,
  EFF_UNKNOWN_INVOKE = 1 << 3,
};

struct EffectSummary {
  // Currently, DCE only checks if a method has EFF_NONE -- otherwise it is
  // never removable. It doesn't dig into the specific reasons for the side
  // effects.
  size_t effects{EFF_NONE};
  std::unordered_set<param_idx_t> modified_params;

  EffectSummary() = default;

  EffectSummary(size_t effects,
                const std::initializer_list<param_idx_t>& modified_params)
      : effects(effects), modified_params(modified_params) {}

  EffectSummary(const std::initializer_list<param_idx_t>& modified_params)
      : modified_params(modified_params) {}

  friend bool operator==(const EffectSummary& a, const EffectSummary& b) {
    return a.effects == b.effects && a.modified_params == b.modified_params;
  }

  static EffectSummary from_s_expr(const sparta::s_expr&);
};

sparta::s_expr to_s_expr(const EffectSummary&);

using EffectSummaryMap = std::unordered_map<const DexMethodRef*, EffectSummary>;

/*
 * Get the effect summary for a single code item.
 */
EffectSummary analyze_code_effects(
    const EffectSummaryMap& effect_summaries,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
    const local_pointers::FixpointIterator& ptrs_fp_iter,
    MethodRefCache*,
    const IRCode*);

/*
 * Get the effect summary for all methods in scope.
 */
void summarize_all_method_effects(
    const Scope& scope,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
    EffectSummaryMap* effect_summaries);
