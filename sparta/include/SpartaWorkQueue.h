/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <boost/optional/optional.hpp>
#include <chrono>
#include <mutex>
#include <numeric>
#include <queue>
#include <random>
#include <thread>

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

static std::atomic_uint num_non_empty{0};
static std::atomic_uint num_running{0};

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

} // namespace workqueue_impl

template <class Input>
class SpartaWorkerState {
 public:
  SpartaWorkerState(size_t id) : m_id(id) {}

  /*
   * Add more items to the queue of the currently-running worker. When a
   * SpartaWorkQueue is running, this should be used instead of
   * SpartaWorkQueue::add_item() as the latter is not thread-safe.
   */
  void push_task(Input task) {
    std::lock_guard<std::mutex> guard(m_queue_mtx);
    if (m_queue.empty()) {
      ++workqueue_impl::num_non_empty;
    }
    m_queue.push(task);
  }

  size_t worker_id() const { return m_id; }

  void set_running(bool running) {
    if (m_running && !running) {
      assert(workqueue_impl::num_running > 0);
      --workqueue_impl::num_running;
    } else if (!m_running && running) {
      ++workqueue_impl::num_running;
    }
    m_running = running;
  };

 private:
  boost::optional<Input> pop_task(SpartaWorkerState<Input>* other) {
    std::lock_guard<std::mutex> guard(m_queue_mtx);
    if (!m_queue.empty()) {
      other->set_running(true);
      if (m_queue.size() == 1) {
        assert(workqueue_impl::num_non_empty > 0);
        --workqueue_impl::num_non_empty;
      }
      auto task = std::move(m_queue.front());
      m_queue.pop();
      return task;
    }
    return boost::none;
  }

  size_t m_id;
  bool m_running{false};
  std::queue<Input> m_queue;
  std::mutex m_queue_mtx;

  template <class>
  friend class SpartaWorkQueue;
};

template <class Input>
class SpartaWorkQueue {
 private:
  using Executor = std::function<void(SpartaWorkerState<Input>*, Input)>;
  Executor m_executor;

  std::vector<std::unique_ptr<SpartaWorkerState<Input>>> m_states;

  const size_t m_num_threads{1};
  size_t m_insert_idx{0};

  void consume(SpartaWorkerState<Input>* state, Input task) {
    m_executor(state, task);
  }

 public:
  SpartaWorkQueue(Executor, unsigned int num_threads);

  void add_item(Input task);

  /**
   * Spawn threads and evaluate function.  This method blocks.
   */
  void run_all();
};

template <class Input>
SpartaWorkQueue<Input>::SpartaWorkQueue(SpartaWorkQueue::Executor executor,
                                        unsigned int num_threads)
    : m_executor(executor), m_num_threads(num_threads) {
  assert(num_threads >= 1);
  for (unsigned int i = 0; i < m_num_threads; ++i) {
    m_states.emplace_back(std::make_unique<SpartaWorkerState<Input>>(i));
  }
}

/**
 * Convenience wrapper for jobs that don't require access to the
 * SpartaWorkerState.
 */
template <class Input>
SpartaWorkQueue<Input> WorkQueue_foreach(
    const std::function<void(Input)>& func,
    unsigned int num_threads = parallel::default_num_threads()) {
  return SpartaWorkQueue<Input>(
      [func](SpartaWorkerState<Input>*, Input a) { func(a); }, num_threads);
}

template <class Input>
void SpartaWorkQueue<Input>::add_item(Input task) {
  m_insert_idx = (m_insert_idx + 1) % m_num_threads;
  assert(m_insert_idx < m_states.size());
  m_states[m_insert_idx]->m_queue.push(task);
}

/*
 * Each worker thread pulls from its own queue first, and then once finished
 * looks randomly at other queues to try and steal work.
 */
template <class Input>
void SpartaWorkQueue<Input>::run_all() {
  std::vector<std::thread> all_threads;
  workqueue_impl::num_non_empty = 0;
  workqueue_impl::num_running = 0;
  auto worker = [&](SpartaWorkerState<Input>* state, size_t state_idx) {
    auto attempts =
        workqueue_impl::create_permutation(m_num_threads, state_idx);
    while (true) {
      auto have_task = false;
      for (auto idx : attempts) {
        auto other_state = m_states[idx].get();
        auto task = other_state->pop_task(state);
        if (task) {
          have_task = true;
          consume(state, *task);
          break;
        }
      }
      if (!have_task) {
        state->set_running(false);
      }
      // Let the thread quit if all the threads are not running and there
      // is no task in any queue.
      if (workqueue_impl::num_running == 0 &&
          workqueue_impl::num_non_empty == 0) {
        return;
      }
    }
  };

  for (size_t i = 0; i < m_num_threads; ++i) {
    if (!m_states[i]->m_queue.empty()) {
      ++workqueue_impl::num_non_empty;
    }
  }
  for (size_t i = 0; i < m_num_threads; ++i) {
    all_threads.emplace_back(std::bind<void>(worker, m_states[i].get(), i));
  }

  for (auto& thread : all_threads) {
    thread.join();
  }
}

} // namespace sparta
