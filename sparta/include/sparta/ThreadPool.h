/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include <sparta/Exceptions.h>

namespace sparta {

// The AsyncRunner provides a way to run work on a separate thread. The main
// thread will not wait for the work to finish; synchronization is up to the
// caller. Sufficiently many threads will get created so that work is never
// blocked.
class AsyncRunner {
 public:
  template <class Function, class... Args>
  void run_async(Function&& f, Args&&... args) {
    run_async_bound(
        std::bind(std::forward<Function>(f), std::forward<Args>(args)...));
  }

  virtual ~AsyncRunner() {}

 protected:
  virtual void run_async_bound(std::function<void()> bound_f) = 0;
};

// The ThreadPool provides an AsyncRunner in a way where threads get reused. A
// sufficient number of threads is going be to created to enable running all
// in-flight async runs concurrently. Destruction of the thread pool waits for
// all async work to finish, and joins all threads.
template <class Thread = std::thread>
class ThreadPool : public AsyncRunner {
 public:
  // Number of spawned unjoined threads.
  size_t size() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_threads.size();
  }

  // Returns true if there are no spawned unjoined threads.
  bool empty() { return size() == 0; }

  // Wait for all async work to finish. Any async work exception will be
  // rethrown here. All threads are joined.
  void join() {
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_all_waiting_cv.wait(lock,
                            [&]() { return m_waiting == m_threads.size(); });
      m_joining = true;
    }
    m_pending_or_joining_cv.notify_all();
    for (auto& thread : m_threads) {
      thread.join();
    }
    m_threads.clear();
    m_waiting = 0;
    auto exception = std::move(m_exception);
    m_joining = false;
    if (exception) {
      std::rethrow_exception(exception);
    }
  }

  // Destruction joins.
  ~ThreadPool() override { join(); }

 protected:
  void run_async_bound(std::function<void()> bound_f) override {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      SPARTA_RUNTIME_CHECK(!m_joining, internal_error());
      if (m_waiting == 0) {
        m_threads.push_back(create_thread(std::move(bound_f)));
        return;
      }
      --m_waiting;
      m_pending.push(std::move(bound_f));
    }
    m_pending_or_joining_cv.notify_one();
  }

  virtual Thread create_thread(std::function<void()> bound_f) {
    return Thread(&ThreadPool::run, this, std::move(bound_f));
  }

  void run(std::function<void()> func) {
    while (true) {
      try {
        func();
      } catch (...) {
        std::unique_lock<std::mutex> lock(m_exception_mutex);
        m_exception = std::current_exception();
      }

      func = nullptr;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (++m_waiting == m_threads.size()) {
          m_all_waiting_cv.notify_one();
        }
        m_pending_or_joining_cv.wait(
            lock, [&]() { return !m_pending.empty() || m_joining; });
        if (m_joining) {
          return;
        }
        func = std::move(m_pending.front());
        m_pending.pop();
      }
    }
  }

 private:
  std::mutex m_exception_mutex;
  std::exception_ptr m_exception;

  std::condition_variable m_pending_or_joining_cv;
  std::condition_variable m_all_waiting_cv;
  std::mutex m_mutex;
  std::vector<Thread> m_threads;
  size_t m_waiting{0};
  std::queue<std::function<void()>> m_pending;
  bool m_joining{false};
};

} // namespace sparta
