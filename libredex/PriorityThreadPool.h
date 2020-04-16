/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <queue>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
#include <boost/asio.hpp>
#include <boost/asio/thread_pool.hpp>
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>

#include "Debug.h"
#include "Thread.h"

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
  std::unique_ptr<boost::asio::thread_pool> m_pool;
  // The following data structures are guarded by this mutex.
  boost::mutex m_mutex;
  std::map<int, std::queue<std::function<void()>>> m_pending_work_items;
  size_t m_running_work_items{0};
  boost::condition_variable m_condition;
  std::chrono::duration<double> m_waited_time;

 public:
  // Creates an instance with a default number of threads
  PriorityThreadPool() {
    set_num_threads(redex_parallel::default_num_threads());
  }

  // Creates an instance with a custom number of threads
  PriorityThreadPool(int num_threads) { set_num_threads(num_threads); }

  ~PriorityThreadPool() {
    // .join() must be manually called before the executor may be destroyed
    always_assert(m_pending_work_items.size() == 0);
  }

  long get_waited_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(m_waited_time)
        .count();
  }

  // The number of threads may be set at most once to a positive number
  void set_num_threads(int num_threads) {
    always_assert(!m_pool);
    if (num_threads > 0) {
      m_pool = std::make_unique<boost::asio::thread_pool>(num_threads);
    }
  }

  // Post a work item with a priority. This method is thread safe.
  void post(int priority, const std::function<void()>& f) {
    always_assert(m_pool);
    {
      boost::mutex::scoped_lock lock(m_mutex);
      m_pending_work_items[priority].push(f);
    }
    boost::asio::defer(*m_pool, [this]() {
      // Find work item with highest priority
      std::function<void()> highest_priority_f;
      {
        boost::mutex::scoped_lock lock(m_mutex);
        auto& p = *m_pending_work_items.rbegin();
        auto& queue = p.second;
        highest_priority_f = queue.front();
        queue.pop();
        if (queue.size() == 0) {
          auto highest_priority = p.first;
          m_pending_work_items.erase(highest_priority);
        }
        m_running_work_items++;
      }
      // Run!
      highest_priority_f();
      // Notify when *all* work is done, i.e. nothing is running or pending.
      {
        boost::mutex::scoped_lock lock(m_mutex);
        if (--m_running_work_items == 0 && m_pending_work_items.size() == 0) {
          m_condition.notify_one();
        }
      }
    });
  }

  // Wait for all work items to be processed.
  void wait() {
    always_assert(m_pool);
    auto start = std::chrono::system_clock::now();
    {
      // We wait until *all* work is done, i.e. nothing is running or pending.
      boost::mutex::scoped_lock lock(m_mutex);
      while (m_running_work_items != 0 || m_pending_work_items.size() != 0) {
        // We'll wait until the condition variable gets notified. Waiting for
        // that will first release the lock, and re-acquire it after the
        // notification came in.
        m_condition.wait(lock);
      }
    }
    auto end = std::chrono::system_clock::now();
    m_waited_time += end - start;
    always_assert(m_pending_work_items.size() == 0);
  }

  void join() {
    wait();
    m_pool->join();
  }
};
