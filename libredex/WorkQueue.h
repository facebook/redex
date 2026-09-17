/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <exception>

#include <sparta/WorkQueue.h>

#include "DeterministicContainers.h"
#include "ThreadPool.h"

namespace redex_workqueue_impl {

void redex_queue_exception_handler(std::exception& e);

// Helper classes so the type of Executor can be inferred
template <typename Input, typename Fn>
struct NoStateWorkQueueHelper {
  Fn fn;
  void operator()(sparta::WorkerState<Input>*, Input a) {
    try {
      fn(std::move(a));
    } catch (std::exception& e) {
      redex_queue_exception_handler(e);
      throw;
    }
  }
};

template <typename Input, typename Fn>
struct WithStateWorkQueueHelper {
  Fn fn;
  void operator()(sparta::WorkerState<Input>* state, Input a) {
    try {
      fn(state, std::move(a));
    } catch (std::exception& e) {
      redex_queue_exception_handler(e);
      throw;
    }
  }
};

} // namespace redex_workqueue_impl

namespace redex_parallel::impl {
constexpr size_t default_num_threads(size_t affinity_concurrency,
                                     size_t hardware_concurrency) {
  const auto available_concurrency =
      affinity_concurrency != 0 ? affinity_concurrency : hardware_concurrency;
  return std::max<size_t>(1, available_concurrency);
}
} // namespace redex_parallel::impl

namespace redex_parallel {
size_t default_num_threads();
} // namespace redex_parallel

// These functions are the most convenient way to create a sparta::WorkQueue
template <class Input,
          typename Fn,
          typename std::enable_if<sparta::Arity<Fn>::value == 1, int>::type = 0>
sparta::WorkQueue<Input,
                  redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>>
workqueue_foreach(const Fn& fn,
                  size_t num_threads = redex_parallel::default_num_threads(),
                  bool push_tasks_while_running = false) {
  return sparta::WorkQueue<
      Input, redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>>(
      redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>{fn}, num_threads,
      push_tasks_while_running, redex_thread_pool::ThreadPool::get_instance());
}
template <class Input,
          typename Fn,
          typename std::enable_if<sparta::Arity<Fn>::value == 2, int>::type = 0>
sparta::WorkQueue<Input,
                  redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>>
workqueue_foreach(const Fn& fn,
                  size_t num_threads = redex_parallel::default_num_threads(),
                  bool push_tasks_while_running = false) {
  return sparta::WorkQueue<
      Input, redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>>(
      redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>{fn},
      num_threads, push_tasks_while_running,
      redex_thread_pool::ThreadPool::get_instance());
}

template <class Input,
          typename Fn,
          typename Items,
          typename std::enable_if<sparta::Arity<Fn>::value == 1, int>::type = 0>
void workqueue_run(const Fn& fn,
                   Items& items,
                   size_t num_threads = redex_parallel::default_num_threads(),
                   bool push_tasks_while_running = false) {
  auto wq = sparta::WorkQueue<
      Input, redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>>(
      redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>{fn}, num_threads,
      push_tasks_while_running, redex_thread_pool::ThreadPool::get_instance());
  for (Input item : UnorderedIterable(items)) {
    wq.add_item(std::move(item));
  }
  wq.run_all();
}
template <class Input,
          typename Fn,
          typename Items,
          typename std::enable_if<sparta::Arity<Fn>::value == 1, int>::type = 0>
void workqueue_run(const Fn& fn,
                   const Items& items,
                   size_t num_threads = redex_parallel::default_num_threads(),
                   bool push_tasks_while_running = false) {
  auto wq = sparta::WorkQueue<
      Input, redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>>(
      redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>{fn}, num_threads,
      push_tasks_while_running, redex_thread_pool::ThreadPool::get_instance());
  for (Input item : UnorderedIterable(items)) {
    wq.add_item(std::move(item));
  }
  wq.run_all();
}
template <class Input,
          typename Fn,
          typename Items,
          typename std::enable_if<sparta::Arity<Fn>::value == 2, int>::type = 0>
void workqueue_run(const Fn& fn,
                   Items& items,
                   size_t num_threads = redex_parallel::default_num_threads(),
                   bool push_tasks_while_running = false) {
  auto wq = sparta::WorkQueue<
      Input, redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>>(
      redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>{fn},
      num_threads, push_tasks_while_running,
      redex_thread_pool::ThreadPool::get_instance());
  for (Input item : UnorderedIterable(items)) {
    wq.add_item(std::move(item));
  }
  wq.run_all();
}
template <class Input,
          typename Fn,
          typename Items,
          typename std::enable_if<sparta::Arity<Fn>::value == 2, int>::type = 0>
void workqueue_run(const Fn& fn,
                   const Items& items,
                   size_t num_threads = redex_parallel::default_num_threads(),
                   bool push_tasks_while_running = false) {
  auto wq = sparta::WorkQueue<
      Input, redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>>(
      redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>{fn},
      num_threads, push_tasks_while_running,
      redex_thread_pool::ThreadPool::get_instance());
  for (Input item : UnorderedIterable(items)) {
    wq.add_item(std::move(item));
  }
  wq.run_all();
}
template <class InteralType, typename Fn>
void workqueue_run_for(
    InteralType start,
    InteralType end,
    const Fn& fn,
    size_t num_threads = redex_parallel::default_num_threads()) {
  auto wq = sparta::WorkQueue<
      InteralType,
      redex_workqueue_impl::NoStateWorkQueueHelper<InteralType, Fn>>(
      redex_workqueue_impl::NoStateWorkQueueHelper<InteralType, Fn>{fn},
      num_threads,
      /* push_tasks_while_running */ false,
      redex_thread_pool::ThreadPool::get_instance());
  for (InteralType i = start; i < end; i++) {
    wq.add_item(i);
  }
  wq.run_all();
}
