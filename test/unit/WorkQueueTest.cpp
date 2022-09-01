/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WorkQueue.h"

#include <array>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <random>

#include "Macros.h"

constexpr unsigned int NUM_STRINGS = 100'000;
constexpr unsigned int NUM_INTS = 1000;

//==========
// Test for correctness. Duplicate of SpartaWorkQueue to check that the
// Redex layer is functional.
//==========

TEST(WorkQueueTest, EmptyQueue) {
  std::atomic<size_t> inv{0};
  auto wq = workqueue_foreach<std::string>(
      [&inv](const std::string& a ATTRIBUTE_UNUSED) {
        inv += 1;
        return 0;
      });
  wq.run_all();
  EXPECT_EQ(0u, inv.load());
}

TEST(WorkQueueTest, EmptyQueueRun) {
  std::atomic<size_t> inv{0};
  workqueue_run<std::string>(
      [&inv](const std::string& a ATTRIBUTE_UNUSED) { inv += 1; },
      std::vector<std::string>{});
  EXPECT_EQ(0u, inv.load());
}

TEST(WorkQueueTest, foreachTest) {
  std::array<int, NUM_INTS> array{};

  auto wq = workqueue_foreach<int*>([](int* a) { (*a)++; });

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(&array[idx]);
  }
  wq.run_all();

  for (const auto& e : array) {
    EXPECT_EQ(1, e);
  }
}

TEST(WorkQueueTest, RunTest) {
  std::array<int, NUM_INTS> array{};

  std::vector<int*> items;
  items.reserve(NUM_INTS);
  std::transform(array.begin(), array.end(), std::back_inserter(items),
                 [](auto& i) { return &i; });

  workqueue_run<int*>([](int* a) { (*a)++; }, items);

  for (const auto& e : array) {
    EXPECT_EQ(1, e);
  }
}

TEST(WorkQueueTest, interal) {
  std::array<int, NUM_INTS> array{};

  workqueue_run_for<int>(0, NUM_INTS, [&array](int i) { array[i]++; });

  for (const auto& e : array) {
    EXPECT_EQ(1, e);
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
