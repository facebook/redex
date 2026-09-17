/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WorkQueue.h"

#include <iostream>

#include <boost/thread/thread.hpp> // NOLINT

#if defined(__linux__) && !defined(__ANDROID__)
#include <sched.h>
#endif

#include "DebugUtils.h"

namespace {

size_t affinity_concurrency() {
#if defined(__linux__) && !defined(__ANDROID__)
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0) {
    return static_cast<size_t>(CPU_COUNT(&cpus));
  }
#endif
  return 0;
}

} // namespace

namespace redex_parallel {

size_t default_num_threads() {
  // Affinity is fixed before Redex starts.
  static const auto value = impl::default_num_threads(
      affinity_concurrency(), boost::thread::hardware_concurrency());
  return value;
}

} // namespace redex_parallel

namespace redex_workqueue_impl {

void redex_queue_exception_handler(std::exception& e) {
  print_stack_trace(std::cerr, e);
}

} // namespace redex_workqueue_impl
