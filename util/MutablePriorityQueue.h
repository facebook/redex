/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <map>
#include <unordered_map>

#include "Debug.h"

/*
 * Collection type that maintains a set of elements with associated
 * priorities, allowing updating priorities, and enabling efficient
 * retrieval of the element with the highest priority.
 *
 * Limitations:
 * - The same value cannot be present twice (even with a different priority)
 * - No two values can exist in the queue with the same priority at the same
 *   time
 */
template <class Value,
          class Priority,
          class ValueHash = std::hash<Value>,
          class PriorityCompare = std::less<Priority>>
class MutablePriorityQueue {
 private:
  std::map<Priority, Value, PriorityCompare> m_values;
  std::unordered_map<Value, Priority, ValueHash> m_priorities;

 public:
  // Inserts a value with a priority; neither value or priority can already be
  // present.
  void insert(const Value& value, const Priority& priority) {
    auto map_result = m_values.insert({priority, value});
    always_assert(map_result.second);
    auto priorities_result = m_priorities.insert({value, priority});
    always_assert(priorities_result.second);
  }

  // Erases a value that's currently in the queue.
  void erase(const Value& value) {
    auto old_priority_it = m_priorities.find(value);
    always_assert(old_priority_it != m_priorities.end());
    const auto erased = m_values.erase(old_priority_it->second);
    always_assert(erased);
    m_priorities.erase(old_priority_it);
  }

  // Changes the priority of a value. The value must already be in the queue.
  // No current queue element may already have the new priority.
  void update_priority(const Value& value, const Priority& priority) {
    erase(value);
    insert(value, priority);
  }

  // Removes all elements.
  void clear() {
    m_values.clear();
    m_priorities.clear();
  }

  // Checks if queue is empty.
  bool empty() const { return m_values.empty(); }

  // Returns element with highest priority.
  Value front() const { return m_values.rbegin()->second; }

  // Returns element with lowest priority.
  Value back() const { return m_values.begin()->second; }
};
