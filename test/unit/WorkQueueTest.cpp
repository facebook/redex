/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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

// Check that we can dynamically adding work items during execution.
TEST(WorkQueueTest, checkDynamicallyAddingTasks) {
  using WorkerState = WorkerState<int, int>;
  WorkQueue<int, int> wq(
      [&wq](WorkerState* worker_state, int a) {
        if (a > 0) {
          worker_state->push_task(a - 1);
          return a;
        }
        return 0;
      },
      [](int a, int b) { return a + b; }, 3);
  wq.add_item(10);
  auto result = wq.run_all();

  // 10 + 9 + ... + 1 + 0 = 55
  EXPECT_EQ(55, result);
}
