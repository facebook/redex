/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/WorkQueue.h>

#include <array>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <random>

constexpr unsigned int NUM_INTS = 1000;

//==========
// Test for correctness
//==========

TEST(WorkQueueTest, EmptyQueue) {
  auto wq = sparta::work_queue<std::string>(
      [](std::string /* unused */) { return 0; });
  wq.run_all();
}

TEST(WorkQueueTest, foreachTest) {
  std::array<int, NUM_INTS> array = {0};

  auto wq = sparta::work_queue<int*>([](int* a) { (*a)++; });

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(&array[idx]);
  }
  wq.run_all();
  for (int idx = 0; idx < NUM_INTS; ++idx) {
    ASSERT_EQ(1, array[idx]);
  }
}

TEST(WorkQueueTest, singleThreadTest) {
  std::array<int, NUM_INTS> array = {0};

  auto wq = sparta::work_queue<int*>([](int* a) { (*a)++; }, 1);

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(&array[idx]);
  }
  wq.run_all();
  for (int idx = 0; idx < NUM_INTS; ++idx) {
    ASSERT_EQ(1, array[idx]);
  }
}

TEST(WorkQueueTest, startFromOneTest) {
  std::array<int, NUM_INTS> array = {0};

  auto wq = sparta::work_queue<int*>([](int* a) { (*a)++; }, 1);

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(&array[idx]);
  }
  wq.run_all();
  for (int idx = 0; idx < NUM_INTS; ++idx) {
    ASSERT_EQ(1, array[idx]);
  }
}

// Check that we can dynamically add work items during execution.
TEST(WorkQueueTest, checkDynamicallyAddingTasks) {
  constexpr size_t num_threads{3};
  std::atomic<int> result{0};
  auto wq = sparta::work_queue<int>(
      [&](sparta::WorkerState<int>* worker_state, int a) {
        if (a > 0) {
          worker_state->push_task(a - 1);
          result += a;
        }
      },
      num_threads,
      /*push_tasks_while_running=*/true);
  wq.add_item(10);
  wq.run_all();

  // 10 + 9 + ... + 1 + 0 = 55
  EXPECT_EQ(55, result);
}

TEST(WorkQueueTest, preciseScheduling) {
  std::array<int, NUM_INTS> array = {0};

  auto wq = sparta::work_queue<int*>([](int* a) { (*a)++; });

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(&array[idx], /* worker_id */ 0);
  }
  wq.run_all();
  for (int idx = 0; idx < NUM_INTS; ++idx) {
    ASSERT_EQ(1, array[idx]);
  }
}

TEST(WorkQueueTest, exceptionPropagation) {
  auto wq = sparta::work_queue<int>([](int i) {
    if (i == 666) {
      throw std::logic_error("exception!");
    }
  });

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(idx);
  }
  ASSERT_THROW(wq.run_all(), std::logic_error);
}

TEST(WorkQueueTest, multipleExceptions) {
  auto wq = sparta::work_queue<int>([](int i) {
    if (i % 3 == 0) {
      throw std::logic_error("exception!");
    }
  });

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(idx);
  }
  ASSERT_THROW(wq.run_all(), std::logic_error);
}
