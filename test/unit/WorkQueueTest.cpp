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
// Test for correctness. Duplicate of SpartaWorkQueue to check that the
// Redex layer is functional.
//==========

TEST(WorkQueueTest, EmptyQueue) {
  auto wq =
      workqueue_foreach<std::string>([](const std::string& a) { return 0; });
  wq.run_all();
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
  constexpr size_t num_threads{3};
  auto results = std::make_unique<int[]>(num_threads);
  auto wq = workqueue_foreach<int>(
      [&](sparta::SpartaWorkerState<int>* worker_state, int a) {
        if (a > 0) {
          worker_state->push_task(a - 1);
          results[worker_state->worker_id()] += a;
        }
      },
      num_threads,
      /*push_tasks_while_running=*/true);
  wq.add_item(10);
  wq.run_all();

  size_t result{0};
  for (size_t i = 0; i < num_threads; ++i) {
    result += results[i];
  }

  // 10 + 9 + ... + 1 + 0 = 55
  EXPECT_EQ(55, result);
}
