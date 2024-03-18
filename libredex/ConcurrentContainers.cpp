/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConcurrentContainers.h"

#include "WorkQueue.h"

namespace cc_impl {

bool is_thread_pool_active() {
  return redex_thread_pool::ThreadPool::get_instance() != nullptr;
}

void workqueue_run_for(size_t start,
                       size_t end,
                       const std::function<void(size_t)>& fn) {
  ::workqueue_run_for<size_t>(start, end, fn);
}

size_t get_prime_number_greater_or_equal_to(size_t value) {
  // We'll choose prime numbers that are roughly doubling in size, but are also
  // always at least 3 less than the next power of two. This allows fitting the
  // allocated Storage into a memory chunk that's just under the next power of
  // two.
  static std::array<uint32_t, 27> primes = {
      13,       29,       61,        113,       251,       509,        1021,
      2039,     4093,     8179,      16381,     32749,     65521,      131063,
      262139,   524269,   1048573,   2097143,   4194301,   8388593,    16777213,
      33554393, 67108859, 134217689, 268435399, 536870909, 1073741789,
  };
  value >>= 3;
  size_t idx = 0;
  while (value >>= 1) {
    idx++;
  }
  if (idx >= primes.size()) {
    // Not necessarily a prime number, too bad.
    return ((size_t)1 << (idx + 4)) - 1;
  }
  return primes[idx];
};

} // namespace cc_impl
