/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "WorkQueue.h"

#include <chrono>
#include <gtest/gtest.h>
#include <random>

constexpr unsigned int NUM_STRINGS = 100'000;
constexpr unsigned int NUM_INTS = 1000;

//==========
// Test for correctness
//==========

TEST(WorkQueueTest, EmptyQueue) {
  auto wq = workqueue_mapreduce<std::string, int>(
      [](std::string a) { return a.size(); },
      [](int a, int b) { return a + b; });
  wq.run_all();
}

TEST(WorkQueueTest, mapreduceTest) {
  auto wq = workqueue_mapreduce<std::string, int>(
      [](std::string a) { return a.size(); },
      [](int a, int b) { return a + b; });

  for (int idx = 0; idx < NUM_STRINGS; ++idx) {
    wq.add_item("wow");
    wq.add_item("abc4");
  }
  int result = wq.run_all();
  ASSERT_EQ(7 * NUM_STRINGS, result);
}

TEST(WorkQueueTest, foreachTest) {
  int array[NUM_INTS] = {0};

  auto wq = workqueue_foreach<int*>([](int* a) { (*a)++; });

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(&array[idx]);
  }
  wq.run_all();
  for (int idx = 0; idx < NUM_INTS; ++idx) {
    ASSERT_EQ(1, array[idx]);
  }
}

TEST(WorkQueueTest, singleThreadTest) {
  int array[NUM_INTS] = {0};

  auto wq = workqueue_foreach<int*>([](int* a) { (*a)++; }, 1);

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(&array[idx]);
  }
  wq.run_all();
  for (int idx = 0; idx < NUM_INTS; ++idx) {
    ASSERT_EQ(1, array[idx]);
  }
}

// use the reference data pointers to compute values, instead of pure inputs.
TEST(WorkQueueTest, checkDataInitialization) {
  int data_refs[3] = {50, 50, 50};
  int array[NUM_INTS] = {0};

  WorkQueue<int, int*, int> wq([](int*& a, int b) { return *a; },
                               [](int a, int b) { return a + b; },
                               [&](int a) { return &data_refs[a]; },
                               3);

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(array[idx]);
  }

  int result = wq.run_all();
  EXPECT_EQ(50 * NUM_INTS, result);
}

// Check that we can dynamically adding work items during execution.
TEST(WorkQueueTest, checkDynamicallyAddingTasks) {
  auto wq = workqueue_mapreduce<int, int>([](int a) { return a; },
                                          [](int a, int b) { return a + b; });
  wq.set_mapper([&wq](std::nullptr_t&, int a) {
    if (a > 0) {
      wq.add_item(a - 1);
      return a;
    }
    return 0;
  });
  wq.add_item(10);
  auto result = wq.run_all();

  // 10 + 9 + ... + 1 + 0 = 55
  EXPECT_EQ(55, result);
}
