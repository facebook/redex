/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InlineForSpeed.h"

#include "ControlFlow.h"
#include "IRCode.h"
#include "MethodProfiles.h"
#include "Resolver.h"

#include <queue>

namespace inline_for_speed {

using namespace method_profiles;

std::unordered_set<const DexMethodRef*> compute_hot_methods(
    const std::unordered_map<const DexMethodRef*, Stats>&
        method_profile_stats) {
  if (method_profile_stats.empty()) {
    return {};
  }
  // Methods in the top PERCENTILE of call counts will be considered hot.
  constexpr double PERCENTILE = 0.1;
  constexpr size_t MIN_SIZE = 1;
  auto result_size = std::max(
      MIN_SIZE, static_cast<size_t>(method_profile_stats.size() * PERCENTILE));
  // Find the lowest score that is in the top PERCENTILE
  // the "top" of the top_scores is actually the minimum hot score
  std::priority_queue<double, std::vector<double>, std::greater<double>>
      high_scores;
  for (const auto& entry : method_profile_stats) {
    auto score = entry.second.call_count;
    if (high_scores.size() < result_size) {
      high_scores.push(score);
    } else if (score > high_scores.top()) {
      high_scores.push(score);
      high_scores.pop();
    }
  }
  auto minimum_hot_score = high_scores.top();
  TRACE(METH_PROF, 2, "minimum hot score = %f", minimum_hot_score);

  // Find all methods with a score higher than the minimum hot score
  std::unordered_set<const DexMethodRef*> result;
  result.reserve(result_size);
  for (const auto& entry : method_profile_stats) {
    const auto& meth = entry.first;
    const auto& stats = entry.second;
    if (stats.call_count >= minimum_hot_score) {
      result.emplace(meth);
    }
  }
  return result;
}

bool should_inline(const DexMethod* caller_method,
                   const DexMethod* callee_method,
                   const std::unordered_set<const DexMethodRef*>& hot_methods) {
  if (hot_methods.empty()) {
    return false;
  }

  if (!(hot_methods.count(caller_method) && hot_methods.count(callee_method))) {
    return false;
  }

  auto callee_insns = callee_method->get_code()->cfg().num_opcodes();
  auto caller_insns = caller_method->get_code()->cfg().num_opcodes();
  constexpr double FUDGE_FACTOR = 0.8; // lowering usually increases # insns
  constexpr size_t ON_DEVICE_COMPILE_MAX = 10000 * FUDGE_FACTOR; // instructions
  if (caller_insns < ON_DEVICE_COMPILE_MAX &&
      caller_insns + callee_insns >= ON_DEVICE_COMPILE_MAX) {
    // Don't push any methods over the on-device compilation limit
    return false;
  }

  auto caller_class = type_class(caller_method->get_class());
  auto callee_class = type_class(callee_method->get_class());
  if (callee_class != nullptr && callee_class != caller_class) {
    auto callee_clinit = callee_class->get_clinit();
    if (callee_clinit != nullptr && callee_clinit->get_code() != nullptr &&
        callee_clinit->get_code()->cfg().num_opcodes() > 0) {
      // Exclude callees with a nontrivial clinit into another class.
      // To avoid a CNF. Maybe our inlining caused a clinit to not get called?
      return false;
    }
  }

  return true;
}

} // namespace inline_for_speed
