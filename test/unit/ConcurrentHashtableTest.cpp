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
    auto ptr = set.get(i);
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
    auto erased = set.erase(i);
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
    auto ptr = set.get(i);
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
