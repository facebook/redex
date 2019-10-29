/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Debug.h"

#include <algorithm>
#include <boost/optional/optional.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <chrono>
#include <numeric>
#include <queue>
#include <random>

// As far as Intel processors are concerned...
#define CACHE_LINE_SIZE 64

namespace redex_parallel {

/**
 * Redex uses the number of physical cores.
 */
static inline unsigned int default_num_threads() {
  unsigned int threads = boost::thread::physical_concurrency();
  return std::max(1u, threads);
}

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

template <class Input, class Output = std::nullptr_t>
class WorkerState {
 public:
  WorkerState(size_t id) : m_id(id) {}

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
  Output m_result;

  template <class, class>
  friend class WorkQueue;
};

template <class Input, class Output = std::nullptr_t>
class WorkQueue {
 private:
  using Mapper = std::function<Output(WorkerState<Input, Output>*, Input)>;
  Mapper m_mapper;
  std::function<Output(Output, Output)> m_reducer;

  std::vector<std::unique_ptr<WorkerState<Input, Output>>> m_states;

  const size_t m_num_threads{1};
  size_t m_insert_idx{0};

  void consume(WorkerState<Input, Output>* state, Input task) {
    state->m_result = m_reducer(state->m_result, m_mapper(state, task));
  }

 public:
  WorkQueue(Mapper mapper,
            std::function<Output(Output, Output)> reducer,
            unsigned int num_threads);

  void add_item(Input task);

  /**
   * Spawn threads and evaluate function.  This method blocks.
   */
  Output run_all(const Output& init_output = Output());
};

template <class Input, class Output>
WorkQueue<Input, Output>::WorkQueue(
    WorkQueue::Mapper mapper,
    std::function<Output(Output, Output)> reducer,
    unsigned int num_threads)
    : m_mapper(mapper), m_reducer(reducer), m_num_threads(num_threads) {
  always_assert(num_threads >= 1);
  for (unsigned int i = 0; i < m_num_threads; ++i) {
    m_states.emplace_back(std::make_unique<WorkerState<Input, Output>>(i));
  }
}

/**
 * Creates a new work queue that doesn't return a value.  This is for
 * jobs that only have side-effects.
 */
template <class Input>
WorkQueue<Input, std::nullptr_t /*Output*/> workqueue_foreach(
    const std::function<void(Input)>& func,
    unsigned int num_threads = redex_parallel::default_num_threads()) {
  using Output = std::nullptr_t;
  return WorkQueue<Input, Output>(
      [func](WorkerState<Input, Output>*, Input a) -> Output {
        func(a);
        return nullptr;
      },
      [](Output, Output) -> Output { return nullptr; },
      num_threads);
}

/**
 * Creates a new work queue that reduces the items to a single value (e.g
 * for a statistics map).  This implies no thread state is required.
 */
template <class Input, class Output>
WorkQueue<Input, Output> workqueue_mapreduce(
    const std::function<Output(Input)>& mapper,
    const std::function<Output(Output, Output)>& reducer,
    unsigned int num_threads = redex_parallel::default_num_threads()) {
  return WorkQueue<Input, Output>(
      [mapper](WorkerState<Input, Output>*, Input a) -> Output {
        return mapper(a);
      },
      reducer,
      num_threads);
}

template <class Input, class Output>
void WorkQueue<Input, Output>::add_item(Input task) {
  m_insert_idx = (m_insert_idx + 1) % m_num_threads;
  m_states[m_insert_idx]->m_queue.push(task);
}

/*
 * Each worker thread pulls from its own queue first, and then once finished
 * looks randomly at other queues to try and steal work.
 */
template <class Input, class Output>
Output WorkQueue<Input, Output>::run_all(const Output& init_output) {
  std::vector<boost::thread> all_threads;
  auto worker = [&](WorkerState<Input, Output>* state, size_t state_idx) {
    workqueue_impl::set_worker_id(state_idx);
    state->m_result = init_output;
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

  Output result = init_output;
  for (auto& thread_state : m_states) {
    result = m_reducer(result, thread_state->m_result);
  }
  return result;
}
