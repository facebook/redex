/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>
#include <unordered_map>
#include <unordered_set>

#include "MethodClosures.h"
#include "MutablePriorityQueue.h"
#include "ReducedControlFlow.h"

namespace method_splitting_impl {

constexpr uint64_t INFREQUENT_COUNT = 11;

// A collection of closures dynamically ordered in a way such that in front is a
// closure that tends to share many components with earlier erased closures,
// while only adding few new components. THis is useful to find a set of similar
// closures, in the sense that they share many components, while not using other
// components. (Inspired by CrossDexRefMinimizer)
class ClosureAggregator {
  std::unordered_set<const ReducedBlock*> m_critical_components;
  MutablePriorityQueue<const Closure*, uint64_t> m_prioritized_closures;
  std::unordered_set<const ReducedBlock*> m_applied_components;
  struct ClosureInfo {
    std::unordered_set<const ReducedBlock*> components;
    uint32_t index;
    uint32_t code_size{0};
    uint32_t applied_code_size{0};
    std::array<uint32_t, INFREQUENT_COUNT> infrequent_code_sizes{};
    uint64_t get_primary_priority_denominator() const;
    uint64_t get_priority() const;
  };
  std::unordered_map<const Closure*, ClosureInfo> m_closure_infos;
  uint32_t m_next_index{0};
  std::unordered_map<const ReducedBlock*, std::unordered_set<const Closure*>>
      m_component_closures;

  struct ClosureInfoDelta {
    std::array<int32_t, INFREQUENT_COUNT> infrequent_code_sizes{};
    int32_t applied_code_size{0};
  };
  using AffectedClosures = std::unordered_map<const Closure*, ClosureInfoDelta>;

  void reprioritize(const AffectedClosures& affected_closures);

 public:
  explicit ClosureAggregator(
      std::unordered_set<const ReducedBlock*> critical_components);
  void insert(const Closure* c);
  bool empty() const { return m_prioritized_closures.empty(); }
  const Closure* front() const { return m_prioritized_closures.front(); }
  void erase(const Closure* c);
};

} // namespace method_splitting_impl
