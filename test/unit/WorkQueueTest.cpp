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

//==========
// Test for performance
//==========

template <typename T>
double calculate_speedup(std::vector<int>& wait_times, int num_threads) {
  auto wq = workqueue_mapreduce<int, int>(
      [](int a) {
        std::this_thread::sleep_for(T(a));
        return a;
      },
      [](int a, int b) { return a + b; },
      num_threads);

  for (auto& item : wait_times) {
    wq.add_item(item);
  }

  auto single_start = std::chrono::high_resolution_clock::now();
  auto sum = 0;
  for (auto& item : wait_times) {
    std::this_thread::sleep_for(T(item));
    sum += item;
  }
  auto single_end = std::chrono::high_resolution_clock::now();

  auto para_start = std::chrono::high_resolution_clock::now();
  auto para_sum = wq.run_all();
  auto para_end = std::chrono::high_resolution_clock::now();

  EXPECT_EQ(sum, para_sum);

  double duration1 =
      std::chrono::duration_cast<T>(single_end - single_start).count();
  double duration2 =
      std::chrono::duration_cast<T>(para_end - para_start).count();
  double speedup = duration1 / duration2;
  return speedup;
}

TEST(WorkQueueTest, profileBusyLoop) {
  std::vector<int> times;
  for (int i = 0; i < 1000; ++i) {
    times.push_back(20);
  }
  double speedup = calculate_speedup<std::chrono::milliseconds>(
      times, std::thread::hardware_concurrency());
  printf("speedup: %f\n", speedup);
  EXPECT_GT(speedup, 0.90 * std::thread::hardware_concurrency());
}

TEST(WorkQueueTest, variableLengthTasks) {
  std::vector<int> times;
  for (int i = 0; i < 50; ++i) {
    auto secs = rand() % 1000;
    times.push_back(secs);
  }
  double speedup = calculate_speedup<std::chrono::milliseconds>(times, 8);
  printf("speedup: %f\n", speedup);
  EXPECT_GT(speedup, 6.0);
}

TEST(WorkQueueTest, smallLengthTasks) {
  std::vector<int> times;
  for (int i = 0; i < 1000; ++i) {
    times.push_back(10);
  }
  double speedup = calculate_speedup<std::chrono::microseconds>(times, 8);
  printf("speedup: %f\n", speedup);
  EXPECT_GT(speedup, 2.0);
}
