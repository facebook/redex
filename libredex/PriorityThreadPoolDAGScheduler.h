/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cinttypes>

#include "ConcurrentContainers.h"
#include "DeterministicContainers.h"
#include "PriorityThreadPool.h"

template <class Task>
class PriorityThreadPoolDAGScheduler {
  using Executor = std::function<void(Task)>;

 private:
  PriorityThreadPool m_priority_thread_pool;
  Executor m_executor;
  UnorderedMap<Task, UnorderedSet<Task>> m_waiting_for;
  UnorderedMap<Task, std::atomic<uint32_t>> m_wait_counts;
  std::unique_ptr<UnorderedMap<Task, int>> m_priorities;
  int m_max_priority{-1};
  using Continuations = std::vector<std::function<void()>>;
  std::unique_ptr<ConcurrentMap<Task, Continuations>>
      m_concurrent_continuations;

  int compute_priority(Task task) {
    auto [it, success] = m_priorities->emplace(task, 0);
    auto& priority = it->second;
    if (!success) {
      return priority;
    }
    auto it2 = m_waiting_for.find(task);
    if (it2 != m_waiting_for.end()) {
      for (auto other_task : UnorderedIterable(it2->second)) {
        priority = std::max(priority, compute_priority(other_task) + 1);
      }
    }
    m_max_priority = std::max(m_max_priority, priority);
    return priority;
  }

  uint32_t increment_wait_count(Task task, uint32_t count = 1) {
    return m_wait_counts.at(task).fetch_add(count);
  }

  void push_back_continuation(Task task, std::function<void()> f) {
    m_concurrent_continuations->update(
        task,
        [f = std::move(f)](Task, Continuations& continuations, bool) mutable {
          continuations.push_back(std::move(f));
        });
  }

  void decrement_wait_count(Task task) {
    if (m_wait_counts.at(task).fetch_sub(1) != 1) {
      return;
    }
    auto* continuations = m_concurrent_continuations->get_and_erase(task);
    if (continuations) {
      // Since the current wait-count is 0, there are not other threads that may
      // read from or append to the continuations of this task.
      always_assert(!continuations->empty());
      auto priority = m_priorities->at(task);
      auto wait_count = increment_wait_count(task, continuations->size());
      always_assert(wait_count == 0);
      for (auto& f : *continuations) {
        m_priority_thread_pool.post(priority, [this, task, f = std::move(f)] {
          f();
          decrement_wait_count(task);
        });
      }
      return;
    }

    auto it = m_waiting_for.find(task);
    if (it == m_waiting_for.end()) {
      return;
    }

    for (auto waiting_task : UnorderedIterable(it->second)) {
      if (m_wait_counts.at(waiting_task).fetch_sub(1) == 1) {
        schedule(waiting_task);
      }
    }
    it->second = decltype(it->second)();
  }

  void schedule(Task task) {
    auto priority = m_priorities->at(task);
    auto wait_count = increment_wait_count(task);
    always_assert(wait_count == 0);
    m_priority_thread_pool.post(priority, [this, task] {
      m_executor(task);
      decrement_wait_count(task);
    });
  }

 public:
  explicit PriorityThreadPoolDAGScheduler(
      Executor executor = [](Task) {},
      int num_threads = redex_parallel::default_num_threads())
      : m_priority_thread_pool(num_threads), m_executor(executor) {}

  void set_executor(Executor executor) { m_executor = std::move(executor); }

  PriorityThreadPool& get_thread_pool() { return m_priority_thread_pool; }

  // The dependency must be scheduled before the task
  void add_dependency(Task task, Task dependency) {
    always_assert(!m_concurrent_continuations);
    m_waiting_for[dependency].insert(task);
    auto& wait_count = m_wait_counts.emplace(task, 0).first->second;
    wait_count.fetch_add(1, std::memory_order_relaxed);
  }

  // While the given task is running, register another function that needs to
  // run before the current task can be considered done. If "continuation" is
  // true, then the given function will only run after all other actions
  // associated with this task have finished running.
  void augment(Task task, std::function<void()> f, bool continuation = false) {
    if (continuation) {
      push_back_continuation(task, std::move(f));
      return;
    }
    auto priority = m_priorities->at(task);
    auto wait_count = increment_wait_count(task);
    always_assert(wait_count > 0);
    m_priority_thread_pool.post(priority, [this, task, f = std::move(f)] {
      f();
      decrement_wait_count(task);
    });
  }

  template <class Collection>
  uint32_t run(Collection collection) {
    always_assert(!m_concurrent_continuations);
    m_priorities = std::make_unique<UnorderedMap<Task, int>>();
    {
      std::vector<std::vector<Task>> ready_tasks;
      for (auto task : collection) {
        int priority = compute_priority(task);
        auto& wait_count = m_wait_counts.emplace(task, 0).first->second;
        if (wait_count.load(std::memory_order_relaxed) != 0) {
          continue;
        }
        always_assert(priority >= 0);
        if ((uint32_t)priority >= ready_tasks.size()) {
          ready_tasks.resize(priority * 2 + 1);
        }
        ready_tasks[priority].push_back(task);
      }
      m_concurrent_continuations =
          std::make_unique<ConcurrentMap<Task, Continuations>>();
      for (size_t i = ready_tasks.size(); i > 0; --i) {
        for (auto task : ready_tasks[i - 1]) {
          schedule(task);
        }
      }
      collection = decltype(collection)();
    }
    m_priority_thread_pool.join();
    always_assert(m_concurrent_continuations->empty());
    m_concurrent_continuations = nullptr;
    for (auto& p : UnorderedIterable(m_wait_counts)) {
      always_assert(p.second.load(std::memory_order_relaxed) == 0);
    }
    m_wait_counts = decltype(m_wait_counts)();
    m_waiting_for = decltype(m_waiting_for)();
    m_priorities = nullptr;
    auto max_priority = m_max_priority;
    m_max_priority = 0;
    return max_priority;
  }
};
