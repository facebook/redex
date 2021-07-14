/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "PriorityThreadPool.h"
#include <unordered_set>

template <class Task>
class PriorityThreadPoolDAGScheduler {
  using Executor = std::function<void(Task)>;

 private:
  PriorityThreadPool m_priority_thread_pool;
  Executor m_executor;
  std::unordered_map<Task, std::unordered_set<Task>> m_waiting_for;
  std::unordered_map<Task, uint> m_wait_counts;
  std::unique_ptr<std::unordered_map<Task, int>> m_priorities;
  int m_max_priority{-1};
  struct ConcurrentState {
    uint wait_count{0};
    std::vector<std::function<void()>> continuations{};
  };
  std::unique_ptr<ConcurrentMap<Task, ConcurrentState>> m_concurrent_states;

  int compute_priority(Task task) {
    auto it = m_priorities->find(task);
    if (it != m_priorities->end()) {
      return it->second;
    }
    auto value = 0;
    auto it2 = m_waiting_for.find(task);
    if (it2 != m_waiting_for.end()) {
      for (auto other_task : it2->second) {
        value = std::max(value, compute_priority(other_task) + 1);
      }
    }
    m_priorities->emplace(task, value);
    m_max_priority = std::max(m_max_priority, value);
    return value;
  }

  uint increment_wait_count(Task task, uint count = 1) {
    uint res = 0;
    m_concurrent_states->update(
        task, [&res, count](Task, ConcurrentState& state, bool) {
          res = state.wait_count;
          state.wait_count += count;
        });
    return res;
  }

  uint push_back_continuation(Task task, std::function<void()> f) {
    uint res = 0;
    m_concurrent_states->update(task,
                                [f, &res](Task, ConcurrentState& state, bool) {
                                  state.continuations.push_back(f);
                                  res = state.wait_count;
                                });
    return res;
  }

  void decrement_wait_count(Task task) {
    bool task_ready = false;
    std::vector<std::function<void()>> continuations;
    m_concurrent_states->update(
        task,
        [&task_ready, &continuations](Task, ConcurrentState& state, bool) {
          if (--state.wait_count == 0) {
            task_ready = true;
            continuations = std::move(state.continuations);
          }
        });
    if (!task_ready) {
      return;
    }

    if (!continuations.empty()) {
      auto priority = m_priorities->at(task);
      increment_wait_count(task, continuations.size());
      for (auto& f : continuations) {
        m_priority_thread_pool.post(priority, [this, task, f = std::move(f)] {
          f();
          decrement_wait_count(task);
        });
      }
      return;
    }

    auto it = m_waiting_for.find(task);
    if (it != m_waiting_for.end()) {
      for (auto waiting_task : m_waiting_for.at(task)) {
        bool waiting_task_ready = false;
        m_concurrent_states->update(
            waiting_task,
            [&waiting_task_ready](Task, ConcurrentState& state, bool) {
              if (--state.wait_count == 0) {
                waiting_task_ready = true;
              }
            });
        if (waiting_task_ready) {
          schedule(waiting_task);
        }
      }
    }
  }

  void schedule(Task task) {
    auto priority = m_priorities->at(task);
    increment_wait_count(task);
    m_priority_thread_pool.post(priority, [this, task] {
      m_executor(task);
      decrement_wait_count(task);
    });
  }

 public:
  explicit PriorityThreadPoolDAGScheduler(
      Executor executor,
      int num_threads = redex_parallel::default_num_threads())
      : m_priority_thread_pool(num_threads), m_executor(executor) {}

  PriorityThreadPool& get_thread_pool() { return m_priority_thread_pool; }

  // The dependency must be scheduled before the task
  void add_dependency(Task task, Task dependency) {
    always_assert(!m_concurrent_states);
    m_waiting_for[dependency].insert(task);
    ++m_wait_counts[task];
  }

  // While the given task is running, register another function that needs to
  // run before the current task can be considered done. If "continuation" is
  // true, then the given function will only run after all other actions
  // associated with this task have finished running.
  void augment(Task task, std::function<void()> f, bool continuation = false) {
    if (continuation) {
      auto active = push_back_continuation(task, std::move(f));
      always_assert(active);
      return;
    }
    auto active = increment_wait_count(task);
    always_assert(active);
    auto priority = m_priorities->at(task);
    m_priority_thread_pool.post(priority, [this, task, f = std::move(f)] {
      f();
      decrement_wait_count(task);
    });
  }

  template <class ForwardIt>
  uint run(const ForwardIt& begin, const ForwardIt& end) {
    always_assert(!m_concurrent_states);
    m_priorities = std::make_unique<std::unordered_map<Task, int>>();
    for (auto it = begin; it != end; it++) {
      compute_priority(*it);
    }
    for (auto& p : *m_priorities) {
      auto it = m_wait_counts.find(p.first);
      p.second =
          (p.second << 16) + (it == m_wait_counts.end() ? 0 : it->second);
    }

    m_concurrent_states =
        std::make_unique<ConcurrentMap<Task, ConcurrentState>>();
    for (auto& p : m_wait_counts) {
      m_concurrent_states->emplace(p.first, (ConcurrentState){p.second});
    }
    std::vector<Task> tasks(begin, end);
    std::stable_sort(tasks.begin(),
                     tasks.end(),
                     [priorities = m_priorities.get()](Task a, Task b) {
                       return priorities->at(a) > priorities->at(b);
                     });
    for (auto task : tasks) {
      if (!m_wait_counts.count(task)) {
        schedule(task);
      }
    }
    m_wait_counts.clear();
    m_priority_thread_pool.join();
    for (auto& p : *m_concurrent_states) {
      always_assert(p.second.wait_count == 0);
      always_assert(p.second.continuations.empty());
    }
    m_concurrent_states = nullptr;
    m_waiting_for.clear();
    m_priorities = nullptr;
    auto max_priority = m_max_priority;
    m_max_priority = 0;
    return max_priority;
  }
};
