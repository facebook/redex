/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConcurrentContainers.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/thread/thread.hpp> // NOLINT

class AtomicMapTest : public ::testing::Test {};

// AtomicMap holds std::atomic values and therefore cannot be copied, only
// moved.
static_assert(!std::is_copy_constructible_v<AtomicMap<size_t, size_t>>);
static_assert(!std::is_copy_assignable_v<AtomicMap<size_t, size_t>>);
static_assert(std::is_move_constructible_v<AtomicMap<size_t, size_t>>);
static_assert(std::is_move_assignable_v<AtomicMap<size_t, size_t>>);

TEST_F(AtomicMapTest, concurrentFetchAdd) {
  const size_t N_THREADS = 1000;
  const size_t N = 100000;
  AtomicMap<uint32_t, uint32_t> map;
  std::vector<boost::thread> threads;
  threads.reserve(N_THREADS);
  for (size_t t = 0; t < N_THREADS; ++t) {
    threads.emplace_back([&]() {
      for (size_t i = 0; i < N; ++i) {
        map.fetch_add(i, 1);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(N, map.size());
  for (size_t i = 0; i < N; ++i) {
    auto value = map.load(i);
    EXPECT_EQ(value, N_THREADS);
  }
}

TEST_F(AtomicMapTest, concurrentStore) {
  const size_t N_THREADS = 1000;
  const size_t N = 100000;
  AtomicMap<size_t, size_t> map;
  std::vector<boost::thread> threads;
  threads.reserve(N_THREADS);
  for (size_t t = 0; t < N_THREADS; ++t) {
    threads.emplace_back([&]() {
      for (size_t i = 0; i < N; ++i) {
        map.store(i, i);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(N, map.size());
  for (size_t i = 0; i < N; ++i) {
    auto value = map.load(i);
    EXPECT_EQ(value, i);
  }
}

TEST_F(AtomicMapTest, exchange) {
  const size_t N = 100000;
  AtomicMap<size_t, size_t> map;
  for (size_t i = 0; i < N; ++i) {
    map.store(i, i);
  }
  for (size_t i = 0; i < N; ++i) {
    auto old = map.exchange(i, N);
    EXPECT_EQ(old, i);
  }
  for (size_t i = 0; i < N; ++i) {
    auto current = map.load(i);
    EXPECT_EQ(current, N);
  }
}

TEST_F(AtomicMapTest, concurrentCompareExchange) {
  const size_t N_THREADS = 1000;
  const size_t N = 100000;
  AtomicMap<size_t, size_t> map;
  std::vector<boost::thread> threads;
  threads.reserve(N_THREADS);
  for (size_t t = 0; t < N_THREADS; ++t) {
    threads.emplace_back([&]() {
      for (size_t i = 0; i < N; ++i) {
        size_t expected = map.load(i);
        while (!map.compare_exchange(i, expected, expected + 1)) {
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(N, map.size());
  for (size_t i = 0; i < N; ++i) {
    auto value = map.load(i);
    EXPECT_EQ(value, N_THREADS);
  }
}

// Regression: fetch_sub must create a missing entry from default_value and then
// atomically subtract, returning the previous value.
TEST_F(AtomicMapTest, fetchSub) {
  AtomicMap<size_t, int64_t> map;
  EXPECT_EQ(0, map.fetch_sub(1, 5)); // creates default 0, then subtracts 5
  EXPECT_EQ(-5, map.load(1));
  EXPECT_EQ(-5, map.fetch_sub(1, 3));
  EXPECT_EQ(-8, map.load(1));
}

// Regression: fetch_and/fetch_or/fetch_xor must create a missing entry from
// default_value and then atomically apply the bitwise op, returning the
// previous value.
TEST_F(AtomicMapTest, fetchBitwise) {
  AtomicMap<size_t, uint32_t> map;
  EXPECT_EQ(0u, map.fetch_or(1, 0b1010u)); // creates default 0
  EXPECT_EQ(0b1010u, map.load(1));
  EXPECT_EQ(0b1010u, map.fetch_and(1, 0b1100u));
  EXPECT_EQ(0b1000u, map.load(1));
  EXPECT_EQ(0b1000u, map.fetch_xor(1, 0b1111u));
  EXPECT_EQ(0b0111u, map.load(1));
}

// Regression: the rvalue overload store(Key&&, Value&&) must compile and both
// insert and overwrite in place at a stable storage location.
TEST_F(AtomicMapTest, storeRvalue) {
  AtomicMap<std::string, size_t> map;
  auto* p1 = map.store(std::string("key"), size_t{1});
  ASSERT_NE(p1, nullptr);
  EXPECT_EQ(1u, map.load("key"));
  auto* p2 = map.store(std::string("key"), size_t{2}); // overwrite path
  EXPECT_EQ(2u, map.load("key"));
  EXPECT_EQ(p1, p2); // storage location is stable across overwrite
}
