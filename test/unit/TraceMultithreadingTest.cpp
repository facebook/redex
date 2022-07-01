/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/thread/thread.hpp>
#include <gtest/gtest.h>
#include <vector>

#include "Trace.h"

constexpr size_t NUM_THREADS = 10;
constexpr size_t NUM_ITERS = 1'000;

TEST(TraceMultithreadingTest, singleThread) { TRACE(TIME, 1, "Test output!"); }

TEST(TraceMultithreadingTest, multipleThreadsOnePrint) {
  std::vector<boost::thread> threads;
  for (size_t idx = 0; idx < NUM_THREADS; ++idx) {
    threads.emplace_back(
        boost::thread([]() { TRACE(TIME, 1, "Test output!"); }));
  }
  for (auto& thread : threads) {
    thread.join();
  }
}

TEST(TraceMultithreadingTest, multipleThreadsMultiplePrints) {
  std::vector<boost::thread> threads;
  for (size_t idx = 0; idx < NUM_THREADS; ++idx) {
    threads.emplace_back(boost::thread([]() {
      for (int j = 0; j < NUM_ITERS; ++j) {
        TRACE(TIME, 1, "Test output count %d", j);
      }
    }));
  }
  for (auto& thread : threads) {
    thread.join();
  }
}

TEST(TraceMultithreadingTest, localThreadContext) {
  std::vector<boost::thread> threads;
  for (size_t idx = 0; idx < NUM_THREADS; ++idx) {
    threads.emplace_back(boost::thread([]() {
      for (int j = 0; j < NUM_ITERS; ++j) {
        std::string context_str = "thread context";
        TraceContext context(&context_str);
        TRACE(TIME, 1, "Test output count %d", j);
        TRACE(TIME, 1, "Test output count %d", j);
        TRACE(TIME, 1, "Test output count %d", j);
      }
    }));
  }
  for (auto& thread : threads) {
    thread.join();
  }
}
