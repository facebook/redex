/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace iodi {

inline constexpr size_t kMaxInflatedSize = size_t{2} * 1024 * 1024;
inline constexpr size_t kMaxBucketInflatedSize = size_t{8} * 1024;

// One emitted IODI debug program. It maps the instruction offsets [0, size)
// of every method assigned to it onto consecutive lines, so dexlayout inflates
// `size * user_count` entries for it.
struct Program {
  uint32_t size{0};
  uint32_t user_count{0};
  // Bytes `DexDebugItem::encode` writes for this program.
  size_t encoded_size{0};
};

// Programs and method assignments for one arity.
//
// `program_of_method` follows the input order. Entries before
// `first_iodi_index` are `kNoProgram`; later entries index a sufficiently
// large entry in `programs`. Equal-sized programs remain distinct.
//
// If programs are not required, `programs` is empty, `first_iodi_index` is 0,
// and every method maps to `kNoProgram`; callers emit no debug info.
struct Plan {
  static constexpr size_t kNoProgram = std::numeric_limits<size_t>::max();

  std::vector<Program> programs;
  std::vector<size_t> program_of_method;
  size_t first_iodi_index{0};
  // Entries dexlayout inflates for this arity: the sum of `size * user_count`
  // over `programs`.
  size_t total_inflated_footprint{0};
  // Debug bytes this plan costs: the emitted programs plus the normal debug
  // programs of the excluded methods.
  size_t total_debug_size{0};
};

// Plans the IODI programs for the methods of one arity. `method_sizes` holds
// their instruction sizes, ordered from largest to smallest, and
// `normal_debug_sizes` the encoded size of each method's normal debug program,
// in the same order. `param_size` and `line_start` are the encoding parameters
// shared by every program of the arity.
//
// `kMaxBucketInflatedSize` bounds each individual program, except that a
// method larger than that bound gets a program of its own, and
// `max_inflated_size` bounds their total.
//
// Chooses the lowest encoded-debug cost among suffixes that satisfy both
// limits. It does not optimize over arbitrary subsets. Equal-size boundaries
// may be rebalanced to reduce inflation without changing encoded size.
//
// When `requires_iodi_programs` is false no program is emitted, so there is no
// inflation to bound and no method to exclude.
Plan plan_programs(std::span<const uint32_t> method_sizes,
                   std::span<const uint64_t> normal_debug_sizes,
                   uint32_t param_size,
                   uint32_t line_start,
                   bool requires_iodi_programs,
                   size_t max_inflated_size = kMaxInflatedSize);

} // namespace iodi
