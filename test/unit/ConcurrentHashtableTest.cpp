/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConcurrentContainers.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>

#include <boost/thread/thread.hpp> // NOLINT

using namespace cc_impl;

class ConcurrentHashtableTest : public ::testing::Test {};

TEST_F(ConcurrentHashtableTest, sequentialInsertGet) {
  const size_t N = 10000;
  ConcurrentHashtable<uint32_t, uint32_t, std::hash<uint32_t>,
                      std::equal_to<uint32_t>>
      set;
  for (size_t i = 0; i < N; ++i) {
    auto insertion_result = set.try_insert(i);
    EXPECT_TRUE(insertion_result.success);
    EXPECT_NE(nullptr, insertion_result.stored_value_ptr);
    EXPECT_EQ(i, *insertion_result.stored_value_ptr);
  }
  EXPECT_EQ(N, set.size());
  for (size_t i = 0; i < N; ++i) {
    auto insertion_result = set.try_insert(i);
    EXPECT_FALSE(insertion_result.success);
    EXPECT_NE(nullptr, insertion_result.stored_value_ptr);
    EXPECT_EQ(i, *insertion_result.stored_value_ptr);
  }
  EXPECT_EQ(N, set.size());
  for (size_t i = 0; i < N; ++i) {
    auto* ptr = set.get(i);
    EXPECT_NE(nullptr, ptr);
    EXPECT_EQ(i, *ptr);
  }
  EXPECT_EQ(nullptr, set.get(N));
}

TEST_F(ConcurrentHashtableTest, sequentialInsertEraseGet) {
  const size_t N = 10000;
  ConcurrentHashtable<uint32_t, uint32_t, std::hash<uint32_t>,
                      std::equal_to<uint32_t>>
      set;
  for (size_t i = 0; i < N; ++i) {
    auto insertion_result = set.try_insert(i);
    EXPECT_TRUE(insertion_result.success);
    EXPECT_NE(nullptr, insertion_result.stored_value_ptr);
    EXPECT_EQ(i, *insertion_result.stored_value_ptr);
  }
  EXPECT_EQ(N, set.size());
  for (size_t i = 0; i < N; ++i) {
    auto* erased = set.erase(i);
    EXPECT_TRUE(erased);
  }
  EXPECT_TRUE(set.empty());
  EXPECT_EQ(nullptr, set.get(0));
}

TEST_F(ConcurrentHashtableTest, concurrentInsertGet) {
  const size_t N_THREADS = 1000;
  const size_t N = 100000;
  ConcurrentHashtable<uint32_t, uint32_t, std::hash<uint32_t>,
                      std::equal_to<uint32_t>>
      set;
  std::vector<boost::thread> threads;
  threads.reserve(N_THREADS);
  for (size_t t = 0; t < N_THREADS; ++t) {
    threads.emplace_back([&]() {
      for (size_t i = 0; i < N; ++i) {
        auto insertion_result = set.try_insert(i);
        EXPECT_NE(nullptr, insertion_result.stored_value_ptr);
        EXPECT_EQ(i, *insertion_result.stored_value_ptr);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(N, set.size());
  for (size_t i = 0; i < N; ++i) {
    auto* ptr = set.get(i);
    EXPECT_NE(nullptr, ptr);
    EXPECT_EQ(i, *ptr);
  }
  EXPECT_EQ(nullptr, set.get(N));
}

TEST_F(ConcurrentHashtableTest, primeProgression) {
  size_t i = 5;
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 13);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 29);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 61);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 113);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 251);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 509);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 1021);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 2039);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 4093);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 8179);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 16381);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 32749);

  i = 1073741789;
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 2147483647);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 4294967295);
  i = cc_impl::get_prime_number_greater_or_equal_to(i * 2);
  ASSERT_EQ(i, 8589934591);
}

using PerturbTable = ConcurrentHashtable<uint32_t,
                                         uint32_t,
                                         std::hash<uint32_t>,
                                         std::equal_to<uint32_t>>;

// A move transplants storage without rehashing, so the destination must adopt
// the source's salt; otherwise (under perturbation) a post-move get() would
// recompute a mismatched bucket and miss. Guards the move-ctor/move-assign salt
// propagation. Correct in both build modes; only load-bearing when perturbation
// is on.
TEST_F(ConcurrentHashtableTest, movePreservesSaltSoLookupsSucceed) {
  constexpr uint32_t N = 10000;

  PerturbTable src;
  for (uint32_t i = 0; i < N; ++i) {
    src.try_insert(i);
  }
  ASSERT_EQ(N, src.size());

  // Move-construct: destination adopts the source's storage and salt.
  PerturbTable moved_ctor = std::move(src);
  ASSERT_EQ(N, moved_ctor.size());
  for (uint32_t i = 0; i < N; ++i) {
    auto* ptr = moved_ctor.get(i);
    ASSERT_NE(nullptr, ptr) << "missing after move-ctor: " << i;
    EXPECT_EQ(i, *ptr);
  }

  // Move-assign into an already-populated destination.
  PerturbTable dst;
  for (uint32_t i = 0; i < 500; ++i) {
    dst.try_insert(1000000 + i);
  }
  dst = std::move(moved_ctor);
  ASSERT_EQ(N, dst.size());
  for (uint32_t i = 0; i < N; ++i) {
    auto* ptr = dst.get(i);
    ASSERT_NE(nullptr, ptr) << "missing after move-assign: " << i;
    EXPECT_EQ(i, *ptr);
  }
}

// N far exceeds the initial table size, forcing several resizes; regardless of
// perturbation, iteration must visit every inserted element exactly once (no
// duplicates, no skips) and every key must remain findable.
TEST_F(ConcurrentHashtableTest, iterationVisitsEveryElementExactlyOnce) {
  constexpr uint32_t N = 10000;
  PerturbTable set;
  for (uint32_t i = 0; i < N; ++i) {
    set.try_insert(i);
  }
  ASSERT_EQ(N, set.size());

  std::unordered_set<uint32_t> seen;
  size_t count = 0;
  for (uint32_t v : set) {
    EXPECT_TRUE(seen.insert(v).second) << "element visited twice: " << v;
    ++count;
  }
  EXPECT_EQ(N, count);
  EXPECT_EQ(N, seen.size());
  for (uint32_t i = 0; i < N; ++i) {
    EXPECT_NE(nullptr, set.get(i));
  }
}

#if REDEX_PERTURB_UNORDERED
// Build the same table twice, each on a fresh thread with a distinct fixed
// REDEX_PERTURB_SEED. new_salt() captures the base seed in a thread_local on
// first use, so a fresh thread per seed makes each salt stream reproducible and
// independent (see DeterministicContainers.h). With N well above the initial
// size, the two salts re-bucket the elements differently, so the iteration
// orders must differ. Fixed seeds make this deterministic, not flaky.
TEST_F(ConcurrentHashtableTest, perturbationChangesIterationOrderAcrossSeeds) {
  constexpr uint32_t N = 10000;
  auto capture_order = [](const char* seed) {
    std::vector<uint32_t> order;
    boost::thread th([&order, seed]() {
      ::setenv("REDEX_PERTURB_SEED", seed, /* overwrite */ 1);
      PerturbTable set;
      for (uint32_t i = 0; i < N; ++i) {
        set.try_insert(i);
      }
      order.reserve(N);
      for (uint32_t v : set) {
        order.push_back(v);
      }
    });
    th.join();
    return order;
  };

  std::vector<uint32_t> order1 = capture_order("0x1111");
  std::vector<uint32_t> order2 = capture_order("0x2222");
  ::unsetenv("REDEX_PERTURB_SEED");

  ASSERT_EQ(N, order1.size());
  ASSERT_EQ(N, order2.size());

  // Same multiset of elements under both seeds...
  std::vector<uint32_t> sorted1 = order1;
  std::vector<uint32_t> sorted2 = order2;
  std::sort(sorted1.begin(), sorted1.end());
  std::sort(sorted2.begin(), sorted2.end());
  EXPECT_EQ(sorted1, sorted2);

  // ...but a different iteration order under different salts.
  EXPECT_NE(order1, order2);
}
#endif
