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

static void foreachTest(sparta::AsyncRunner* async_runner = nullptr) {
  std::array<int, NUM_INTS> array = {0};

  auto wq = sparta::work_queue<int*>([](int* a) { (*a)++; },
                                     sparta::parallel::default_num_threads(),
                                     /* push_tasks_while_running */ false,
                                     async_runner);

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(&array[idx]);
  }
  wq.run_all();
  for (int idx = 0; idx < NUM_INTS; ++idx) {
    ASSERT_EQ(1, array[idx]);
  }
}

TEST(WorkQueueTest, foreachTest) { foreachTest(); }

TEST(WorkQueueTest, foreachThreadPoolTest) {
  sparta::ThreadPool<> thread_pool;
  foreachTest(&thread_pool);
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

// Similar checkDynamicallyAddingTasks, but do much more work.
TEST(WorkQueueTest, stress) {
  for (size_t num_threads = 8; num_threads <= 128; num_threads *= 2) {
    std::atomic<int> result{0};
    auto wq = sparta::work_queue<int>(
        [&](sparta::WorkerState<int>* worker_state, int a) {
          if (a > 0) {
            worker_state->push_task(a - 1);
            result++;
          }
        },
        num_threads,
        /*push_tasks_while_running=*/true);
    const size_t N = 200;
    for (size_t i = 0; i <= N; i++) {
      wq.add_item(10 * i);
    }
    wq.run_all();

    // 10 * N + 10 * (N - 1) + ... + 10
    // = 10 * (N + (N - 1) + ... + 1 + 0)
    // = 10 * (N * (N + 1) / 2)
    // = 201000 // for N = 200
    EXPECT_EQ(201000, result);
  }
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

static void exceptionPropagation(sparta::AsyncRunner* async_runner = nullptr) {
  auto wq = sparta::work_queue<int>(
      [](int i) {
        if (i == 666) {
          throw std::logic_error("exception!");
        }
      },
      sparta::parallel::default_num_threads(),
      /* push_tasks_while_running */ false, async_runner);

  for (int idx = 0; idx < NUM_INTS; ++idx) {
    wq.add_item(idx);
  }
  ASSERT_THROW(wq.run_all(), std::logic_error);
}

TEST(WorkQueueTest, exceptionPropagation) { exceptionPropagation(); }

TEST(WorkQueueTest, exceptionPropagationThreadPool) {
  sparta::ThreadPool<> thread_pool;
  exceptionPropagation(&thread_pool);
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
