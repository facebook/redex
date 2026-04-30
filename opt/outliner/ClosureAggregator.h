/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <array>

#include "DeterministicContainers.h"
#include "MethodClosures.h"
#include "MutablePriorityQueue.h"
#include "ReducedControlFlow.h"

namespace method_splitting_impl {

constexpr uint64_t INFREQUENT_COUNT = 11;

// A collection of closures dynamically ordered in a way such that in front is a
// closure that tends to share many components with earlier erased closures,
// while only adding few new components. This is useful to find a set of similar
// closures, in the sense that they share many components, while not using other
// components. (Inspired by CrossDexRefMinimizer)
class ClosureAggregator {
  UnorderedSet<const ReducedBlock*> m_critical_components;
  MutablePriorityQueue<const Closure*, uint64_t> m_prioritized_closures;
  UnorderedSet<const ReducedBlock*> m_applied_components;
  struct ClosureInfo {
    UnorderedSet<const ReducedBlock*> components;
    uint32_t index;
    size_t code_size{0};
    size_t applied_code_size{0};
    std::array<size_t, INFREQUENT_COUNT> infrequent_code_sizes{};
    uint64_t get_primary_priority_denominator() const;
    uint64_t get_priority() const;
  };
  UnorderedMap<const Closure*, ClosureInfo> m_closure_infos;
  uint32_t m_next_index{0};
  UnorderedMap<const ReducedBlock*, UnorderedSet<const Closure*>>
      m_component_closures;

  struct ClosureInfoDelta {
    std::array<size_t, INFREQUENT_COUNT> infrequent_code_sizes{};
    size_t applied_code_size{0};
  };
  using AffectedClosures = UnorderedMap<const Closure*, ClosureInfoDelta>;

  void reprioritize(const AffectedClosures& affected_closures);

 public:
  explicit ClosureAggregator(
      UnorderedSet<const ReducedBlock*> critical_components);
  void insert(const Closure* c);
  bool empty() const { return m_prioritized_closures.empty(); }
  const Closure* front() const { return m_prioritized_closures.front(); }
  void erase(const Closure* c);
};

} // namespace method_splitting_impl
