/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClosureAggregator.h"

#include <numeric>

namespace method_splitting_impl {

uint64_t ClosureAggregator::ClosureInfo::get_primary_priority_denominator()
    const {
  always_assert(code_size >= applied_code_size);
  always_assert(code_size >= std::accumulate(infrequent_code_sizes.begin(),
                                             infrequent_code_sizes.end(), 0UL,
                                             [](uint32_t x, uint32_t y) {
                                               return static_cast<uint64_t>(x) +
                                                      static_cast<uint64_t>(y);
                                             }));
  uint32_t unapplied_code_size = code_size - applied_code_size;
  int64_t denominator = static_cast<int64_t>(unapplied_code_size) * 16;
  for (size_t i = 1; i < infrequent_code_sizes.size(); ++i) {
    denominator -=
        static_cast<int64_t>(infrequent_code_sizes[i]) * 16 / (i + 1);
  }
  return static_cast<uint64_t>(std::max(denominator, INT64_C(1)));
}

uint64_t ClosureAggregator::ClosureInfo::get_priority() const {
  uint64_t nominator = applied_code_size + infrequent_code_sizes[0];
  uint64_t denominator = get_primary_priority_denominator();
  uint64_t primary_priority = (nominator << 20) / denominator;
  primary_priority = std::min(primary_priority, (UINT64_C(1) << 40) - 1);

  // We'll certainly have fewer than 1<24 closures.
  always_assert(index < (1 << 24));
  uint32_t secondary_priority = 0xFFFFFF - index;

  // The combined priority is a composite of the primary and secondary
  // priority, where the primary priority is using the top 40 bits, and
  // the secondary priority the low 24 bits.
  return (primary_priority << 24) | secondary_priority;
}

ClosureAggregator::ClosureAggregator(
    std::unordered_set<const ReducedBlock*> critical_components)
    : m_critical_components(std::move(critical_components)) {}

void ClosureAggregator::reprioritize(
    const AffectedClosures& affected_closures) {
  for (auto& [affected_closure, delta] : affected_closures) {
    auto& affected_closure_info = m_closure_infos.at(affected_closure);
    affected_closure_info.applied_code_size += delta.applied_code_size;
    for (size_t i = 0; i < INFREQUENT_COUNT; ++i) {
      affected_closure_info.infrequent_code_sizes[i] +=
          delta.infrequent_code_sizes[i];
    }

    const auto priority = affected_closure_info.get_priority();
    m_prioritized_closures.update_priority(affected_closure, priority);
  }
}

void ClosureAggregator::insert(const Closure* c) {
  std::unordered_set<const ReducedBlock*> components;
  for (auto* component : c->reduced_components) {
    if (m_critical_components.count(component)) {
      components.insert(component);
    }
  }
  auto [it, emplaced] = m_closure_infos.emplace(
      c, (ClosureInfo){std::move(components), m_next_index++});
  always_assert(emplaced);
  auto& closure_info = it->second;

  AffectedClosures affected_closures;
  for (const auto* component : closure_info.components) {
    closure_info.code_size += component->code_size;
    auto& closures = m_component_closures[component];
    size_t frequency = closures.size();
    if (frequency > 0 && frequency <= INFREQUENT_COUNT) {
      for (const Closure* affected_closure : closures) {
        always_assert(affected_closure != c);
        affected_closures[affected_closure]
            .infrequent_code_sizes[frequency - 1] -=
            static_cast<int32_t>(component->code_size);
      }
    }
    ++frequency;
    if (frequency <= INFREQUENT_COUNT) {
      for (const Closure* affected_closure : closures) {
        always_assert(affected_closure != c);
        affected_closures[affected_closure]
            .infrequent_code_sizes[frequency - 1] += component->code_size;
      }
      closure_info.infrequent_code_sizes[frequency - 1] += component->code_size;
    }

    closures.emplace(c);
  }
  const auto priority = closure_info.get_priority();
  m_prioritized_closures.insert(c, priority);
  reprioritize(affected_closures);
}
void ClosureAggregator::erase(const Closure* c) {
  m_prioritized_closures.erase(c);

  AffectedClosures affected_closures;
  auto closure_info_it = m_closure_infos.find(c);
  always_assert(closure_info_it != m_closure_infos.end());
  const auto& closure_info = closure_info_it->second;
  for (const auto* component : closure_info.components) {
    auto closures_it = m_component_closures.find(component);
    always_assert(closures_it != m_component_closures.end());
    auto& closures = closures_it->second;
    size_t frequency = closures.size();
    always_assert(frequency > 0);
    const auto erased = closures.erase(c);
    always_assert(erased);
    if (frequency <= INFREQUENT_COUNT) {
      for (const Closure* affected_closure : closures) {
        affected_closures[affected_closure]
            .infrequent_code_sizes[frequency - 1] -=
            static_cast<int32_t>(component->code_size);
      }
    }
    --frequency;
    if (frequency == 0) {
      m_component_closures.erase(closures_it);
    } else if (frequency <= INFREQUENT_COUNT) {
      for (const Closure* affected_closure : closures) {
        affected_closures[affected_closure]
            .infrequent_code_sizes[frequency - 1] += component->code_size;
      }
    }

    if (m_applied_components.insert(component).second && frequency > 0) {
      for (const Closure* affected_closure : closures) {
        affected_closures[affected_closure].applied_code_size +=
            component->code_size;
      }
    }
  }
  m_closure_infos.erase(closure_info_it);
  reprioritize(affected_closures);
}

} // namespace method_splitting_impl
