/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Debug.h"
#include "Thread.h"

#include <algorithm>
#include <boost/optional/optional.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <chrono>
#include <numeric>
#include <queue>
#include <random>

namespace redex_parallel {

constexpr int INVALID_ID{-1};

/*
 * When an instance of WorkQueue is running, this will return the ID of the
 * worker for the current thread. Unlike std::this_thread::get_id(), these IDs
 * go from 0 to (num workers - 1).
 *
 * Note that only one WorkQueue should be running at any point in time; nested /
 * concurrent instance of WorkQueues are not supported.
 */
int get_worker_id();

} // namespace redex_parallel

namespace workqueue_impl {

void set_worker_id(int);

/**
 * Creates a random ordering of which threads to visit.  This prevents threads
 * from being prematurely emptied (if everyone targets thread 0, for example)
 *
 * Each thread should empty its own queue first, so we explicitly set the
 * thread's index as the first element of the list.
 */
inline std::vector<int> create_permutation(int num, unsigned int thread_idx) {
  std::vector<int> attempts(num);
  std::iota(attempts.begin(), attempts.end(), 0);
  auto seed = std::chrono::system_clock::now().time_since_epoch().count();
  std::shuffle(
      attempts.begin(), attempts.end(), std::default_random_engine(seed));
  std::iter_swap(attempts.begin(),
                 std::find(attempts.begin(), attempts.end(), thread_idx));
  return attempts;
}

} // namespace workqueue_impl

template <class Input>
class WorkerState {
 public:
  explicit WorkerState(size_t id) : m_id(id) {}

  /*
   * Add more items to the queue of the currently-running worker. When a
   * WorkQueue is running, this should be used instead of WorkQueue::add_item()
   * as the latter is not thread-safe.
   */
  void push_task(Input task) {
    boost::lock_guard<boost::mutex> guard(m_queue_mtx);
    m_queue.push(task);
  }

  size_t worker_id() const { return m_id; }

 private:
  boost::optional<Input> pop_task() {
    boost::lock_guard<boost::mutex> guard(m_queue_mtx);
    if (!m_queue.empty()) {
      auto task = std::move(m_queue.front());
      m_queue.pop();
      return task;
    }
    return boost::none;
  }

  size_t m_id;
  std::queue<Input> m_queue;
  boost::mutex m_queue_mtx;

  template <class, typename>
  friend class WorkQueueBase;
};

template <class Input, typename Executor>
class WorkQueueBase {
 protected:
  Executor m_executor;

  std::vector<std::unique_ptr<WorkerState<Input>>> m_states;

  const size_t m_num_threads{1};
  size_t m_insert_idx{0};

  void consume(WorkerState<Input>* state, Input task) {
    m_executor(state, task);
  }

 public:
  WorkQueueBase(Executor executor, unsigned int num_threads)
      : m_executor(executor), m_num_threads(num_threads) {
    always_assert(num_threads >= 1);
    for (unsigned int i = 0; i < m_num_threads; ++i) {
      m_states.emplace_back(std::make_unique<WorkerState<Input>>(i));
    }
  }

  void add_item(Input task) {
    m_insert_idx = (m_insert_idx + 1) % m_num_threads;
    m_states[m_insert_idx]->m_queue.push(task);
  }

  /**
   * Spawn threads and evaluate function.  This method blocks.
   */
  void run_all() {
    std::vector<boost::thread> all_threads;
    auto worker = [&](WorkerState<Input>* state, size_t state_idx) {
      workqueue_impl::set_worker_id(state_idx);
      auto attempts =
          workqueue_impl::create_permutation(m_num_threads, state_idx);
      while (true) {
        auto have_task = false;
        for (auto idx : attempts) {
          auto other_state = m_states[idx].get();
          auto task = other_state->pop_task();
          if (task) {
            have_task = true;
            consume(state, *task);
            break;
          }
        }
        if (!have_task) {
          workqueue_impl::set_worker_id(redex_parallel::INVALID_ID);
          return;
        }
      }
    };

    for (size_t i = 0; i < m_num_threads; ++i) {
      boost::thread::attributes attrs;
      attrs.set_stack_size(8 * 1024 * 1024);
      all_threads.emplace_back(attrs,
                               boost::bind<void>(worker, m_states[i].get(), i));
    }

    for (auto& thread : all_threads) {
      thread.join();
    }
  }
};

// Standard legacy definition.
template <class Input>
using WorkQueue =
    WorkQueueBase<Input, std::function<void(WorkerState<Input>*, Input)>>;

//
// Convenience wrapper for jobs that don't require access to the WorkerState.
//

// This struct is necessary to be able to type the result of workqueue_foreach.
template <typename Input, typename Fn>
struct WorkQueueHelper {
  Fn fn;
  void operator()(WorkerState<Input>* state, Input a) { fn(a); }
};

template <class Input, typename Fn>
WorkQueueBase<Input, WorkQueueHelper<Input, Fn>> workqueue_foreach(
    const Fn& fn,
    unsigned int num_threads = redex_parallel::default_num_threads()) {
  return WorkQueueBase<Input, WorkQueueHelper<Input, Fn>>(
      WorkQueueHelper<Input, Fn>{fn}, num_threads);
}
