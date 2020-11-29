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
#include "Show.h"
#include "Trace.h"

#include <queue>

using namespace method_profiles;

constexpr double MIN_APPEAR_PERCENT = 80.0;

namespace {
bool method_has_try_blocks(const DexMethod* method) {
  const IRCode* code = method->get_code();
  if (code->editable_cfg_built()) {
    auto ii = cfg::ConstInstructionIterable(code->cfg());
    return std::any_of(ii.begin(), ii.end(),
                       [](auto& mie) { return mie.type == MFLOW_TRY; });
  } else {
    return std::any_of(code->begin(), code->end(),
                       [](auto& mie) { return mie.type == MFLOW_TRY; });
  }
}
} // namespace

void InlineForSpeed::compute_hot_methods() {
  if (m_method_profiles == nullptr || !m_method_profiles->has_stats()) {
    return;
  }
  for (const auto& pair : m_method_profiles->all_interactions()) {
    const std::string& interaction_id = pair.first;
    const auto& method_stats = pair.second;
    size_t popular_set_size = 0;
    for (const auto& entry : method_stats) {
      if (entry.second.appear_percent >= MIN_APPEAR_PERCENT) {
        ++popular_set_size;
      }
    }
    // Methods in the top PERCENTILE of call counts will be considered warm/hot.
    constexpr double WARM_PERCENTILE = 0.25;
    constexpr double HOT_PERCENTILE = 0.1;
    // Find the lowest score that is within the given percentile
    constexpr size_t MIN_SIZE = 1;
    size_t warm_size = std::max(
        MIN_SIZE, static_cast<size_t>(popular_set_size * WARM_PERCENTILE));
    size_t hot_size = std::max(
        MIN_SIZE, static_cast<size_t>(popular_set_size * HOT_PERCENTILE));
    // the "top" of the queue is actually the minimum warm/hot score
    using pq =
        std::priority_queue<double, std::vector<double>, std::greater<double>>;
    pq warm_scores;
    pq hot_scores;
    auto maybe_push = [](pq& q, size_t size, double value) {
      if (q.size() < size) {
        q.push(value);
      } else if (value > q.top()) {
        q.push(value);
        q.pop();
      }
    };
    for (const auto& entry : method_stats) {
      const auto& stat = entry.second;
      if (stat.appear_percent >= MIN_APPEAR_PERCENT) {
        auto score = stat.call_count;
        maybe_push(warm_scores, warm_size, score);
        maybe_push(hot_scores, hot_size, score);
      }
    }
    double min_warm_score = std::max(50.0, warm_scores.top());
    double min_hot_score = std::max(100.0, hot_scores.top());
    TRACE(METH_PROF,
          2,
          "%s min scores = %f, %f",
          interaction_id.c_str(),
          min_warm_score,
          min_hot_score);
    std::pair<double, double> p(min_warm_score, min_hot_score);
    m_min_scores.emplace(interaction_id, std::move(p));
  }
}

InlineForSpeed::InlineForSpeed(const MethodProfiles* method_profiles)
    : m_method_profiles(method_profiles) {
  compute_hot_methods();
}

bool InlineForSpeed::enabled() const {
  return m_method_profiles != nullptr && m_method_profiles->has_stats();
}

bool InlineForSpeed::should_inline(const DexMethod* caller_method,
                                   const DexMethod* callee_method) const {
  if (!enabled()) {
    return false;
  }

  // TODO(T80442770): Remove this restriction once the experimentation framework
  // can support methods with exceptions or we're done with the experiment.
  if (method_has_try_blocks(caller_method) ||
      method_has_try_blocks(callee_method)) {
    return false;
  }

  auto caller_insns = caller_method->get_code()->cfg().num_opcodes();
  // The cost of inlining large methods usually outweighs the benefits
  constexpr uint32_t MAX_NUM_INSNS = 240;
  if (caller_insns > MAX_NUM_INSNS) {
    return false;
  }
  auto callee_insns = callee_method->get_code()->cfg().num_opcodes();
  if (callee_insns > MAX_NUM_INSNS) {
    return false;
  }

  // If the pair is hot under any interaction, inline it.
  for (const auto& pair : m_method_profiles->all_interactions()) {
    bool should = should_inline_per_interaction(caller_method,
                                                callee_method,
                                                caller_insns,
                                                callee_insns,
                                                pair.first,
                                                pair.second);
    if (should) {
      return true;
    }
  }
  return false;
}

bool InlineForSpeed::should_inline_per_interaction(
    const DexMethod* caller_method,
    const DexMethod* callee_method,
    uint32_t caller_insns,
    uint32_t callee_insns,
    const std::string& interaction_id,
    const StatsMap& method_stats) const {
  const auto& caller_search = method_stats.find(caller_method);
  if (caller_search == method_stats.end()) {
    return false;
  }
  const auto& scores = m_min_scores.at(interaction_id);
  double warm_score = scores.first;
  double hot_score = scores.second;
  const auto& caller_stats = caller_search->second;
  auto caller_hits = caller_stats.call_count;
  auto caller_appears = caller_stats.appear_percent;
  if (caller_hits < warm_score || caller_appears < MIN_APPEAR_PERCENT) {
    return false;
  }

  const auto& callee_search = method_stats.find(callee_method);
  if (callee_search == method_stats.end()) {
    return false;
  }
  const auto& callee_stats = callee_search->second;
  auto callee_hits = callee_stats.call_count;
  auto callee_appears = callee_stats.appear_percent;
  if (callee_hits < warm_score || callee_appears < MIN_APPEAR_PERCENT) {
    return false;
  }

  // Smaller methods tend to benefit more from inlining. Allow warm + small
  // methods, or hot + medium size methods.
  constexpr uint32_t SMALL_ENOUGH = 20;
  bool either_small =
      caller_insns < SMALL_ENOUGH || callee_insns < SMALL_ENOUGH;
  bool either_hot = caller_hits >= hot_score || callee_hits >= hot_score;
  bool result = either_small || either_hot;
  if (result) {
    TRACE(METH_PROF,
          5,
          "%s, %s, %s, %u, %u, %f, %f",
          SHOW(caller_method),
          SHOW(callee_method),
          interaction_id.c_str(),
          caller_insns,
          callee_insns,
          caller_hits,
          callee_hits);
  }
  return result;
}
