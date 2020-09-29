/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/optional.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "Debug.h"
#include "SpartaWorkQueue.h" // For `default_num_threads`.
#include "WorkQueue.h" // For redex_queue_exception_handler.

/*
 * Individual work items are posted with a priority:
 * - Work items with the highest priority are executed first.
 * - Priorities are signed integers, allowing flexibility for negative
 *   priorities.
 *
 * The thread-pool must be initialized with a positive number of threads to be
 * functional.
 */
class PriorityThreadPool {
 private:
  std::vector<std::thread> m_pool;
  // The following data structures are guarded by this mutex.
  std::mutex m_mutex;
  std::condition_variable m_work_condition;
  std::condition_variable m_done_condition;
  std::map<int, std::queue<std::function<void()>>> m_pending_work_items;
  size_t m_running_work_items{0};
  std::chrono::duration<double> m_waited_time{0};
  bool m_shutdown{false};

 public:
  // Creates an instance with a default number of threads
  PriorityThreadPool() {
    set_num_threads(sparta::parallel::default_num_threads());
  }

  // Creates an instance with a custom number of threads
  explicit PriorityThreadPool(int num_threads) { set_num_threads(num_threads); }

  ~PriorityThreadPool() {
    // If the pool was created (>0 threads), `join` must be manually called
    // before the executor may be destroyed.
    always_assert(m_pending_work_items.empty());
    if (!m_pool.empty()) {
      always_assert(m_shutdown);
      always_assert(m_running_work_items == 0);
    }
  }

  long get_waited_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(m_waited_time)
        .count();
  }

  // The number of threads may be set at most once to a positive number
  void set_num_threads(int num_threads) {
    always_assert(m_pool.empty());
    always_assert(!m_shutdown);
    if (num_threads > 0) {
      // std::thread cannot be copied, so need to do this in a loop instead of
      // `resize`.
      for (size_t i = 0; i != (size_t)num_threads; ++i) {
        m_pool.emplace_back(&PriorityThreadPool::run, this);
      }
    }
  }

  // Post a work item with a priority. This method is thread safe.
  void post(int priority, const std::function<void()>& f) {
    always_assert(!m_pool.empty());
    std::unique_lock<std::mutex> lock{m_mutex};
    always_assert(!m_shutdown);
    m_pending_work_items[priority].push(f);
    m_work_condition.notify_one();
  }

  // Wait for all work items to be processed.
  void wait(bool init_shutdown = false) {
    always_assert(!m_pool.empty());
    auto start = std::chrono::system_clock::now();
    {
      // We wait until *all* work is done, i.e. nothing is running or pending.
      std::unique_lock<std::mutex> lock{m_mutex};
      m_done_condition.wait(lock, [&]() {
        return m_running_work_items == 0 && m_pending_work_items.empty();
      });
      if (init_shutdown) {
        m_shutdown = true;
        m_work_condition.notify_all();
      }
    }
    auto end = std::chrono::system_clock::now();
    m_waited_time += end - start;
  }

  void join(bool allow_new_work = true) {
    always_assert(!m_pool.empty());
    always_assert(!m_shutdown);
    if (!allow_new_work) {
      std::unique_lock<std::mutex> lock{m_mutex};
      m_shutdown = true;
      m_work_condition.notify_all();
    }
    wait(/*init_shutdown=*/allow_new_work);
    for (auto& thread : m_pool) {
      thread.join();
    }
  }

 private:
  void run() {
    for (;;) {
      auto highest_priority_f =
          [&]() -> boost::optional<std::function<void()>> {
        std::unique_lock<std::mutex> lock{m_mutex};
        // Wait for work or shutdown.
        m_work_condition.wait(lock, [&]() {
          return !m_pending_work_items.empty() || m_shutdown;
        });
        if (m_pending_work_items.empty()) {
          redex_assert(m_shutdown);
          return boost::none;
        }

        // Find work item with highest priority
        auto& p = *m_pending_work_items.rbegin();
        auto& queue = p.second;
        auto f = queue.front();
        queue.pop();
        if (queue.empty()) {
          auto highest_priority = p.first;
          m_pending_work_items.erase(highest_priority);
        }
        m_running_work_items++;
        return f;
      }();
      if (!highest_priority_f) {
        return;
      }

      // Run!
      try {
        (*highest_priority_f)();
      } catch (std::exception& e) {
        redex_workqueue_impl::redex_queue_exception_handler(e);
        throw;
      }

      // Notify when *all* work is done, i.e. nothing is running or pending.
      {
        std::unique_lock<std::mutex> lock{m_mutex};
        if (--m_running_work_items == 0 && m_pending_work_items.empty()) {
          m_done_condition.notify_one();
        }
      }
    }
  }
};
