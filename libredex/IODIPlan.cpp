/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IODIPlan.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <vector>

#include "Debug.h"
#include "DexEncoding.h"

namespace iodi {

namespace {

// Bytes `DexDebugItem::encode` writes for a program covering the instruction
// offsets [0, size): the two uleb128 header fields, one byte for each
// parameter's absent name, one byte per line entry, and DBG_END_SEQUENCE.
size_t encoded_program_size(uint32_t size,
                            uint32_t param_size,
                            uint32_t line_start) {
  return uleb128_encoding_size(line_start) + uleb128_encoding_size(param_size) +
         size_t{param_size} + size_t{size} + 1;
}

// A program is as long as the largest method assigned to it, so a program of
// `size` can serve at most kMaxBucketInflatedSize / size methods before
// dexlayout inflates more entries than the per-program bound allows. At least
// one method always fits: a method above that bound still needs a program to
// map its lines.
size_t max_program_users(uint32_t size) {
  if (size == 0) {
    return std::numeric_limits<size_t>::max();
  }
  return std::max<size_t>(1, kMaxBucketInflatedSize / size);
}

} // namespace

Plan plan_programs(std::span<const uint32_t> method_sizes,
                   std::span<const uint64_t> normal_debug_sizes,
                   uint32_t param_size,
                   uint32_t line_start,
                   bool requires_iodi_programs,
                   size_t max_inflated_size) {
  always_assert(method_sizes.size() == normal_debug_sizes.size());
  const size_t num_methods = method_sizes.size();
  Plan plan;
  plan.program_of_method.assign(num_methods, Plan::kNoProgram);
  if (!requires_iodi_programs || num_methods == 0) {
    return plan;
  }

  // First method of the equal-size run containing each method. Sizes descend,
  // so a run is contiguous.
  std::vector<size_t> run_start(num_methods, 0);
  for (size_t i = 0; i < num_methods; ++i) {
    run_start[i] = (i > 0 && method_sizes[i] == method_sizes[i - 1])
                       ? run_start[i - 1]
                       : i;
  }

  // Walking from the largest method down, the program that starts at method i
  // covers a fixed run of methods, so the programs of a suffix are the program
  // starting at its first method followed by the programs of a later suffix.
  // One backwards pass therefore yields the exact inflation footprint and the
  // exact encoded size of every suffix.
  //
  // Serving as many methods as the per-program bound allows minimizes the
  // suffix's bytes: a suffix's cost never rises as its first method is
  // dropped -- drop that method from its program and the program either
  // disappears or shrinks to the next method's size -- so ending this program
  // as late as possible leaves the cheapest remainder.
  std::vector<size_t> next_start(num_methods + 1, num_methods);
  std::vector<size_t> suffix_footprint(num_methods + 1, 0);
  std::vector<size_t> suffix_program_size(num_methods + 1, 0);

  // A program only shrinks by excluding the largest methods that use it, so
  // the candidate plans are the suffixes of `method_sizes` plus the empty one,
  // which emits nothing and always satisfies the aggregate limit. A suffix
  // footprint is not monotonic in where the suffix starts -- dropping the
  // largest method lets the next one absorb more users, which can inflate more
  // -- so every candidate is checked against the limit rather than inheriting
  // the verdict of an earlier one. Each candidate is evaluated as its suffix
  // is completed; ties go to the later candidate, whose programs are smaller
  // and inflate less.
  const size_t total_normal_size = std::accumulate(
      normal_debug_sizes.begin(), normal_debug_sizes.end(), size_t{0});
  size_t suffix_normal_size = 0;
  size_t best_first = num_methods;
  size_t best_size = total_normal_size;
  always_assert_log(
      std::is_sorted(method_sizes.begin(), method_sizes.end(),
                     std::greater<>{}),
      "IODI method sizes must be sorted from largest to smallest");
  for (size_t i = num_methods; i-- > 0;) {
    const size_t greedy_end =
        i + std::min(num_methods - i, max_program_users(method_sizes[i]));
    size_t end = greedy_end;
    // Shift trailing methods to the next program when they match its size.
    // This keeps encoded sizes unchanged while reducing inflation, stopping
    // before the next program fills or the run ends. That run is smaller than
    // method i, so it starts after i and this program keeps method i.
    if (greedy_end < num_methods &&
        method_sizes[greedy_end] < method_sizes[i]) {
      const size_t next_end = next_start[greedy_end];
      const size_t room = max_program_users(method_sizes[greedy_end]);
      const size_t earliest_end = next_end > room ? next_end - room : 0;
      end = std::max(run_start[greedy_end], earliest_end);
    }
    always_assert_log(i < end && end <= greedy_end,
                      "IODI program at %zu must end in (%zu, %zu], got %zu", i,
                      i, greedy_end, end);
    next_start[i] = end;
    const size_t users = end - i;
    suffix_footprint[i] =
        size_t{method_sizes[i]} * users + suffix_footprint[end];
    suffix_program_size[i] =
        encoded_program_size(method_sizes[i], param_size, line_start) +
        suffix_program_size[end];
    suffix_normal_size += normal_debug_sizes[i];
    if (suffix_footprint[i] > max_inflated_size) {
      continue;
    }
    const size_t total_size =
        total_normal_size - suffix_normal_size + suffix_program_size[i];
    if (total_size < best_size) {
      best_size = total_size;
      best_first = i;
    }
  }

  plan.first_iodi_index = best_first;
  plan.total_debug_size = best_size;
  for (size_t i = best_first; i < num_methods; i = next_start[i]) {
    const auto users = static_cast<uint32_t>(next_start[i] - i);
    plan.programs.push_back(
        Program{method_sizes[i], users,
                encoded_program_size(method_sizes[i], param_size, line_start)});
    for (size_t user = i; user != next_start[i]; ++user) {
      plan.program_of_method[user] = plan.programs.size() - 1;
    }
    plan.total_inflated_footprint += size_t{method_sizes[i]} * users;
  }
  redex_assert(plan.total_inflated_footprint == suffix_footprint[best_first]);
  return plan;
}

} // namespace iodi
