/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <boost/optional/optional.hpp>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <thread>
#include <utility>

#include <sparta/Arity.h>

namespace sparta {

namespace parallel {

/**
 * Sparta uses the number of physical cores.
 */
static inline unsigned int default_num_threads() {
  unsigned int threads = std::thread::hardware_concurrency();
  return std::max(1u, threads);
}

} // namespace parallel

namespace workqueue_impl {

/**
 * Creates a random ordering of which threads to visit.  This prevents threads
 * from being prematurely emptied (if everyone targets thread 0, for example)
 *
 * Each thread should empty its own queue first, so we explicitly set the
 * thread's index as the first element of the list.
 */
inline std::vector<unsigned int> create_permutation(unsigned int num,
                                                    unsigned int thread_idx) {
  std::vector<unsigned int> attempts(num);
  std::iota(attempts.begin(), attempts.end(), 0);
  auto seed = std::chrono::system_clock::now().time_since_epoch().count();
  std::shuffle(
      attempts.begin(), attempts.end(), std::default_random_engine(seed));
  std::iter_swap(attempts.begin(),
                 std::find(attempts.begin(), attempts.end(), thread_idx));
  return attempts;
}

class Semaphore {
 public:
  explicit Semaphore(size_t initial = 0u) : m_count(initial) {}

  inline void give(size_t n = 1u) {
    std::unique_lock<std::mutex> lock(m_mtx);
    m_count += n;
    if (n == 1) {
      m_cv.notify_one();
    } else {
      m_cv.notify_all(); // A bit suboptimal, but easier than precise counting.
    }
  }

  inline void take() {
    std::unique_lock<std::mutex> lock(m_mtx);
    while (m_count == 0) {
      m_cv.wait(lock);
    }
    --m_count;
  }

  inline void take_all() {
    std::unique_lock<std::mutex> lock(m_mtx);
    m_count = 0;
  }

 private:
  std::mutex m_mtx;
  std::condition_variable m_cv;
  size_t m_count;
};

struct StateCounters {
  std::atomic_uint num_non_empty;
  std::atomic_uint num_running;
  const unsigned int num_all;
  // Mutexes aren't move-able.
  std::unique_ptr<Semaphore> waiter;

  explicit StateCounters(unsigned int num)
      : num_non_empty(0),
        num_running(0),
        num_all(num),
        waiter(new Semaphore(0)) {}
  StateCounters(StateCounters&& other)
      : num_non_empty(other.num_non_empty.load()),
        num_running(other.num_running.load()),
        num_all(other.num_all),
        waiter(std::move(other.waiter)) {}
};

} // namespace workqueue_impl

template <class Input, typename Executor>
class WorkQueue;

template <class Input>
class WorkerState final {
 public:
  WorkerState(size_t id, workqueue_impl::StateCounters* sc, bool can_push)
      : m_id(id), m_state_counters(sc), m_can_push_task(can_push) {}

  /*
   * Add more items to the queue of the currently-running worker. When a
   * `WorkQueue` is running, this should be used instead of
   * `WorkQueue::add_item()` as the latter is not thread-safe.
   */
  void push_task(Input task) {
    assert(m_can_push_task);
    std::lock_guard<std::mutex> guard(m_queue_mtx);
    if (m_queue.empty()) {
      ++m_state_counters->num_non_empty;
    }
    if (m_state_counters->num_running < m_state_counters->num_all) {
      m_state_counters->waiter->give(1u); // May consider waking all.
    }
    m_queue.push(std::move(task));
  }

  size_t worker_id() const { return m_id; }

  void set_running(bool running) {
    if (m_running && !running) {
      assert(m_state_counters->num_running > 0);
      --m_state_counters->num_running;
    } else if (!m_running && running) {
      ++m_state_counters->num_running;
    }
    m_running = running;
  };

 private:
  boost::optional<Input> pop_task(WorkerState<Input>* other) {
    std::lock_guard<std::mutex> guard(m_queue_mtx);
    if (!m_queue.empty()) {
      other->set_running(true);
      if (m_queue.size() == 1) {
        assert(m_state_counters->num_non_empty > 0);
        --m_state_counters->num_non_empty;
      }
      auto task = std::move(m_queue.front());
      m_queue.pop();
      return boost::optional<Input>(std::move(task));
    }
    return boost::none;
  }

  size_t m_id;
  bool m_running{false};
  std::queue<Input> m_queue;
  std::mutex m_queue_mtx;
  workqueue_impl::StateCounters* m_state_counters;
  const bool m_can_push_task{false};

  template <class, typename>
  friend class WorkQueue;
};

template <class Input, typename Executor>
class WorkQueue {
 private:
  // Using templates for Executor to avoid the performance overhead of
  // std::function
  Executor m_executor;
  std::vector<std::unique_ptr<WorkerState<Input>>> m_states;
  const size_t m_num_threads{1};
  size_t m_insert_idx{0};
  workqueue_impl::StateCounters m_state_counters;
  const bool m_can_push_task{false};

 public:
  WorkQueue(Executor,
            unsigned int num_threads = parallel::default_num_threads(),
            // push_tasks_while_running:
            // * When this flag is true, all threads stay alive until the
            //   last task is finished. Useful when threads are adding
            //   more work to the queue via `WorkerState::push_task`.
            // * When this flag is false, threads can
            //   exit as soon as there is no more work (to avoid
            //   preempting a thread that has useful work)
            bool push_tasks_while_running = false);

  // copies are not allowed
  WorkQueue(const WorkQueue&) = delete;
  // moves are allowed
  WorkQueue(WorkQueue&&) = default;

  /* Adds (a copy of) an item to a pseudo-random worker. */
  void add_item(Input task);

  /* Add an item on the queue of the given worker. */
  void add_item(Input task, size_t worker_id);

  /**
   * Spawn threads and evaluate function.  This method blocks.
   */
  void run_all();

  template <class>
  friend class WorkerState;
};

template <class Input, typename Executor>
WorkQueue<Input, Executor>::WorkQueue(Executor executor,
                                      unsigned int num_threads,
                                      bool push_tasks_while_running)
    : m_executor(executor),
      m_num_threads(num_threads),
      m_state_counters(num_threads),
      m_can_push_task(push_tasks_while_running) {
  assert(num_threads >= 1);
  for (unsigned int i = 0; i < m_num_threads; ++i) {
    m_states.emplace_back(std::make_unique<WorkerState<Input>>(
        i, &m_state_counters, m_can_push_task));
  }
}

template <class Input, typename Executor>
void WorkQueue<Input, Executor>::add_item(Input task) {
  m_insert_idx = (m_insert_idx + 1) % m_num_threads;
  assert(m_insert_idx < m_states.size());
  m_states[m_insert_idx]->m_queue.push(std::move(task));
}

template <class Input, typename Executor>
void WorkQueue<Input, Executor>::add_item(Input task, size_t worker_id) {
  assert(worker_id < m_states.size());
  m_states[worker_id]->m_queue.push(std::move(task));
}

/*
 * Each worker thread pulls from its own queue first, and then once finished
 * looks randomly at other queues to try and steal work.
 */
template <class Input, typename Executor>
void WorkQueue<Input, Executor>::run_all() {
  m_state_counters.num_non_empty = 0;
  m_state_counters.num_running = 0;
  m_state_counters.waiter->take_all();
  std::mutex exception_mutex;
  std::exception_ptr exception;
  auto worker = [&](WorkerState<Input>* state, size_t state_idx) {
    try {
      auto attempts =
          workqueue_impl::create_permutation(m_num_threads, state_idx);
      while (true) {
        auto have_task = false;
        for (auto idx : attempts) {
          auto other_state = m_states[idx].get();
          auto task = other_state->pop_task(state);
          if (task) {
            have_task = true;
            m_executor(state, std::move(*task));
            break;
          }
        }
        if (have_task) {
          continue;
        }

        state->set_running(false);
        if (!m_can_push_task) {
          // New tasks can't be added. We don't need to wait for the currently
          // running jobs to finish.
          return;
        }

        // Let the thread quit if all the threads are not running and there
        // is no task in any queue.
        if (m_state_counters.num_running == 0 &&
            m_state_counters.num_non_empty == 0) {
          // Wake up everyone who might be waiting, so they can quit.
          m_state_counters.waiter->give(m_state_counters.num_all);
          return;
        }

        m_state_counters.waiter->take(); // Wait for work.
      }
    } catch (...) {
      {
        std::unique_lock<std::mutex> lock(exception_mutex);
        if (exception) {
          // An exception was already caught.
          state->set_running(false);
          return;
        }
        exception = std::current_exception();
      }

      // Make all other threads stop gracefully, by stealing their tasks.
      for (unsigned int idx = 0; idx < m_num_threads; idx++) {
        auto other_state = m_states[idx].get();
        while (true) {
          auto task = other_state->pop_task(state);
          if (!task) {
            break;
          }
        }
      }
      state->set_running(false);
      m_state_counters.waiter->give(m_state_counters.num_all);
    }
  };

  for (size_t i = 0; i < m_num_threads; ++i) {
    if (!m_states[i]->m_queue.empty()) {
      ++m_state_counters.num_non_empty;
    }
  }

  std::vector<std::thread> all_threads;
  all_threads.reserve(m_num_threads);
  for (size_t i = 0; i < m_num_threads; ++i) {
    all_threads.emplace_back(worker, m_states[i].get(), i);
  }

  for (auto& thread : all_threads) {
    thread.join();
  }

  for (size_t i = 0; i < m_num_threads; ++i) {
    assert(!m_states[i]->m_running);
  }

  if (exception) {
    std::rethrow_exception(exception);
  }

  for (size_t i = 0; i < m_num_threads; ++i) {
    assert(m_states[i]->m_queue.empty());
  }
}

namespace workqueue_impl {
// Helper classes so the type of Executor can be inferred
template <typename Input, typename Fn>
struct NoStateWorkQueueHelper {
  Fn fn;
  void operator()(WorkerState<Input>*, Input a) { fn(std::move(a)); }
};
template <typename Input, typename Fn>
struct WithStateWorkQueueHelper {
  Fn fn;
  void operator()(WorkerState<Input>* state, Input a) {
    fn(state, std::move(a));
  }
};
} // namespace workqueue_impl

// These functions are the most convenient way to create a WorkQueue
template <class Input,
          typename Fn,
          typename std::enable_if<Arity<Fn>::value == 1, int>::type = 0>
WorkQueue<Input, workqueue_impl::NoStateWorkQueueHelper<Input, Fn>> work_queue(
    const Fn& fn,
    unsigned int num_threads = parallel::default_num_threads(),
    bool push_tasks_while_running = false) {
  return WorkQueue<Input, workqueue_impl::NoStateWorkQueueHelper<Input, Fn>>(
      workqueue_impl::NoStateWorkQueueHelper<Input, Fn>{fn},
      num_threads,
      push_tasks_while_running);
}
template <class Input,
          typename Fn,
          typename std::enable_if<Arity<Fn>::value == 2, int>::type = 0>
WorkQueue<Input, workqueue_impl::WithStateWorkQueueHelper<Input, Fn>>
work_queue(const Fn& fn,
           unsigned int num_threads = parallel::default_num_threads(),
           bool push_tasks_while_running = false) {
  return WorkQueue<Input, workqueue_impl::WithStateWorkQueueHelper<Input, Fn>>(
      workqueue_impl::WithStateWorkQueueHelper<Input, Fn>{fn},
      num_threads,
      push_tasks_while_running);
}

} // namespace sparta
