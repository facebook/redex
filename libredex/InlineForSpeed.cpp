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

InlineForSpeed::InlineForSpeed(
    const std::unordered_map<const DexMethodRef*, method_profiles::Stats>&
        method_profile_stats)
    : m_method_profile_stats(method_profile_stats),
      m_hot_methods(compute_hot_methods(m_method_profile_stats)) {}

bool InlineForSpeed::enabled() const { return !m_hot_methods.empty(); }

bool InlineForSpeed::should_inline(const DexMethod* caller_method,
                                   const DexMethod* callee_method) const {
  if (!enabled()) {
    return false;
  }

  if (!(m_hot_methods.count(caller_method) &&
        m_hot_methods.count(callee_method))) {
    return false;
  }

  auto callee_insns = callee_method->get_code()->cfg().num_opcodes();
  auto caller_insns = caller_method->get_code()->cfg().num_opcodes();
  auto caller_hits = m_method_profile_stats.at(caller_method).call_count;
  auto callee_hits = m_method_profile_stats.at(callee_method).call_count;

  TRACE_NO_LINE(METH_PROF,
                5,
                "%s, %s, %u, %u, %f, %f, ",
                SHOW(caller_method),
                SHOW(callee_method),
                caller_insns,
                callee_insns,
                caller_hits,
                callee_hits);

  constexpr double FUDGE_FACTOR = 0.8; // lowering usually increases # insns
  constexpr size_t ON_DEVICE_COMPILE_MAX = 10000 * FUDGE_FACTOR; // instructions
  if (caller_insns < ON_DEVICE_COMPILE_MAX &&
      caller_insns + callee_insns >= ON_DEVICE_COMPILE_MAX) {
    // Don't push any methods over the on-device compilation limit
    TRACE(METH_PROF, 5, "%d, %s", 0, "ONDEVICE");
    return false;
  }

  double CALLER_BARELY_HOT = 100.0;
  double CALLEE_BARELY_HOT = 200.0;
  if (caller_insns > 12 && callee_insns > 8 &&
      m_method_profile_stats.at(caller_method).call_count < CALLER_BARELY_HOT &&
      m_method_profile_stats.at(callee_method).call_count < CALLEE_BARELY_HOT) {
    TRACE(METH_PROF, 5, "%d, %s", 0, "BARELYHOT");
    return false;
  }
  uint32_t CALLER_MAX_NUM_INSNS = 160;
  if (caller_insns > CALLER_MAX_NUM_INSNS) {
    TRACE(METH_PROF, 5, "%d, %s", 0, "BIGCALLER");
    return false;
  }

  TRACE(METH_PROF, 5, "%d, %s", 1, "");
  return true;
}
