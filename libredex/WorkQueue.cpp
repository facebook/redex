/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WorkQueue.h"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <system_error>

#include <boost/thread/thread.hpp> // NOLINT

#if defined(__linux__) && !defined(__ANDROID__)
#include <sched.h>
#endif

#include "Debug.h"
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

namespace redex_parallel::impl {

std::optional<size_t> parse_max_threads(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  size_t max_threads{0};
  const auto* first = &value.front();
  const auto* last = first + value.size();
  const auto [ptr, ec] = std::from_chars(first, last, max_threads);
  if (ec != std::errc() || ptr != last || max_threads == 0) {
    return std::nullopt;
  }
  return max_threads;
}

} // namespace redex_parallel::impl

namespace redex_parallel {

size_t default_num_threads() {
  // Affinity and the process environment are fixed before Redex starts.
  static const auto value = []() {
    const auto available_threads = impl::default_num_threads(
        affinity_concurrency(), boost::thread::hardware_concurrency());
    const auto* configured_value = std::getenv("REDEX_MAX_THREADS");
    if (configured_value == nullptr) {
      return available_threads;
    }
    const auto configured_max_threads =
        impl::parse_max_threads(configured_value);
    always_assert_log(
        configured_max_threads,
        "Invalid REDEX_MAX_THREADS value `%s`; expected a positive integer",
        configured_value);
    return std::min(*configured_max_threads, available_threads);
  }();
  return value;
}

} // namespace redex_parallel

namespace redex_workqueue_impl {

void redex_queue_exception_handler(std::exception& e) {
  print_stack_trace(std::cerr, e);
}

} // namespace redex_workqueue_impl
