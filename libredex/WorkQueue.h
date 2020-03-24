/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <exception>

#include "SpartaWorkQueue.h"

namespace redex_workqueue_impl {

void redex_queue_exception_handler(std::exception& e);

// Helper classes so the type of Executor can be inferred
template <typename Input, typename Fn>
struct NoStateWorkQueueHelper {
  Fn fn;
  void operator()(sparta::SpartaWorkerState<Input>*, Input a) {
    try {
      fn(a);
    } catch (std::exception& e) {
      redex_queue_exception_handler(e);
      throw;
    }
  }
};

template <typename Input, typename Fn>
struct WithStateWorkQueueHelper {
  Fn fn;
  void operator()(sparta::SpartaWorkerState<Input>* state, Input a) {
    try {
      fn(state, a);
    } catch (std::exception& e) {
      redex_queue_exception_handler(e);
      throw;
    }
  }
};

} // namespace redex_workqueue_impl

// Simple forward to keep changes minimal for the time being.
namespace redex_parallel {
inline size_t default_num_threads() {
  return sparta::parallel::default_num_threads();
}
} // namespace redex_parallel

// These functions are the most convenient way to create a SpartaWorkQueue
template <class Input,
          typename Fn,
          typename std::enable_if<sparta::Arity<Fn>::value == 1, int>::type = 0>
sparta::SpartaWorkQueue<Input,
                        redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>>
workqueue_foreach(
    const Fn& fn,
    unsigned int num_threads = sparta::parallel::default_num_threads(),
    bool push_tasks_while_running = false) {
  return sparta::SpartaWorkQueue<
      Input,
      redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>>(
      redex_workqueue_impl::NoStateWorkQueueHelper<Input, Fn>{fn},
      num_threads,
      push_tasks_while_running);
}
template <class Input,
          typename Fn,
          typename std::enable_if<sparta::Arity<Fn>::value == 2, int>::type = 0>
sparta::SpartaWorkQueue<
    Input,
    redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>>
workqueue_foreach(
    const Fn& fn,
    unsigned int num_threads = sparta::parallel::default_num_threads(),
    bool push_tasks_while_running = false) {
  return sparta::SpartaWorkQueue<
      Input,
      redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>>(
      redex_workqueue_impl::WithStateWorkQueueHelper<Input, Fn>{fn},
      num_threads,
      push_tasks_while_running);
}
