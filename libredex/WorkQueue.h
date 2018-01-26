/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "Debug.h"

#include <algorithm>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <chrono>
#include <numeric>
#include <queue>
#include <random>

namespace workqueue_impl {

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

template <class Input, class Data, class Output>
struct WorkerState {
  std::queue<Input> queue;
  boost::mutex queue_mtx;
  Data data;
  Output result;

  WorkerState(const Data& initial) : data(initial) {}

  bool pop_task(Input& task) {
    boost::lock_guard<boost::mutex> guard(queue_mtx);
    if (!queue.empty()) {
      task = std::move(queue.front());
      queue.pop();
      return true;
    }
    return false;
  }
};

template <class Input, class Data, class Output>
class WorkQueue {
 private:
  bool m_currently_running{false};
  std::function<Output(Data&, Input)> m_mapper;
  std::function<Output(Output, Output)> m_reducer;

  std::vector<std::unique_ptr<WorkerState<Input, Data, Output>>> m_states;

  const size_t m_num_threads{1};
  size_t m_insert_idx{0};

  void consume(WorkerState<Input, Data, Output>* state, Input task) {
    state->result = m_reducer(state->result, m_mapper(state->data, task));
  }

 public:
  WorkQueue(
      std::function<Output(Data&, Input)> mapper,
      std::function<Output(Output, Output)> reducer,
      std::function<Data(unsigned int /* thread index*/)> data_initializer,
      unsigned int num_threads);

  void add_item(Input task);

  void set_mapper(std::function<Output(Data&, Input)> mapper) {
    m_mapper = mapper;
  }

  void set_reducer(std::function<Output(Output&, Output)> reducer) {
    m_reducer = reducer;
  }

  /**
   * Spawn threads and evaluate function.  This method blocks.
   */
  Output run_all(const Output& init_output = Output());
};

template <class Input, class Data, class Output>
WorkQueue<Input, Data, Output>::WorkQueue(
    std::function<Output(Data&, Input)> mapper,
    std::function<Output(Output, Output)> reducer,
    std::function<Data(unsigned int /* thread index */)> data_initializer,
    unsigned int num_threads)
    : m_mapper(mapper), m_reducer(reducer), m_num_threads(num_threads) {
  always_assert(num_threads >= 1);
  for (unsigned int i = 0; i < m_num_threads; ++i) {
    m_states.emplace_back(std::make_unique<WorkerState<Input, Data, Output>>(
        data_initializer(i)));
  }
}

/**
 * Creates a new work queue that doesn't return a value.  This is for
 * jobs that only have side-effects.
 */
template <class Input>
WorkQueue<Input, std::nullptr_t /*Data*/, std::nullptr_t /*Output*/>
workqueue_foreach(const std::function<void(Input)>& func,
                  unsigned int num_threads =
                      std::max(1u, boost::thread::hardware_concurrency())) {
  using Data = std::nullptr_t;
  using Output = std::nullptr_t;
  return WorkQueue<Input, Data, Output>(
      [func](Data&, Input a) -> Output {
        func(a);
        return nullptr;
      },
      [](Data, Data) -> Data { return nullptr; },
      [](unsigned int) -> Data { return nullptr; },
      num_threads);
}

/**
 * Creates a new work queue that reduces the items to a single value (e.g
 * for a statistics map).  This implies no thread state is required.
 */
template <class Input, class Output>
WorkQueue<Input, std::nullptr_t /*Data*/, Output> workqueue_mapreduce(
    const std::function<Output(Input)>& mapper,
    const std::function<Output(Output, Output)>& reducer,
    unsigned int num_threads =
        std::max(1u, boost::thread::hardware_concurrency())) {
  using Data = std::nullptr_t;
  return WorkQueue<Input, Data, Output>(
      [mapper](Data&, Input a) -> Output { return mapper(a); },
      reducer,
      [](unsigned int) -> Data { return nullptr; },
      num_threads);
}

template <class Input, class Data, class Output>
void WorkQueue<Input, Data, Output>::add_item(Input task) {
  if (m_currently_running) {
    auto insert_idx = rand() % m_num_threads;
    boost::lock_guard<boost::mutex> guard(m_states[insert_idx]->queue_mtx);
    m_states[insert_idx]->queue.push(task);
  } else {
    m_insert_idx = (m_insert_idx + 1) % m_num_threads;
    m_states[m_insert_idx]->queue.push(task);
  }
}

/*
 * Each worker thread pulls from its own queue first, and then once finished
 * looks randomly at other queues to try and steal work.
 */
template <class Input, class Data, class Output>
Output WorkQueue<Input, Data, Output>::run_all(const Output& init_output) {
  m_currently_running = true;
  std::vector<boost::thread> all_threads;
  auto worker = [&](WorkerState<Input, Data, Output>* state, size_t state_idx) {
    state->result = init_output;
    auto attempts =
        workqueue_impl::create_permutation(m_num_threads, state_idx);
    while (true) {
      auto have_task = false;
      for (auto idx : attempts) {
        auto other_state = m_states[idx].get();
        Input task;
        have_task = other_state->pop_task(task);
        if (have_task) {
          consume(state, task);
          break;
        }
      }
      if (!have_task) {
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
    result = m_reducer(result, thread_state->result);
  }
  m_currently_running = false;
  return result;
}
