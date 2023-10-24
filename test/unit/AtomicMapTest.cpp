/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConcurrentContainers.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/thread/thread.hpp>

class AtomicMapTest : public ::testing::Test {};

TEST_F(AtomicMapTest, concurrentFetchAdd) {
  const size_t N_THREADS = 1000;
  const size_t N = 100000;
  AtomicMap<uint32_t, uint32_t> map;
  std::vector<boost::thread> threads;
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
