/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WorkQueue.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <gtest/gtest.h>
#include <optional>
#include <string_view>

#if defined(__linux__) && !defined(__ANDROID__)
#include <sched.h>
#endif

#include "Macros.h"
#include "RedexException.h"

constexpr unsigned int NUM_INTS = 1000;

TEST(WorkQueueTest, DefaultNumThreadsPrefersAffinity) {
  EXPECT_EQ(16, redex_parallel::impl::default_num_threads(16, 32));
  EXPECT_EQ(32, redex_parallel::impl::default_num_threads(0, 32));
  EXPECT_EQ(1, redex_parallel::impl::default_num_threads(0, 0));
}

TEST(WorkQueueTest, ParseMaxThreads) {
  EXPECT_EQ(36, redex_parallel::impl::parse_max_threads("36"));
  EXPECT_EQ(std::nullopt, redex_parallel::impl::parse_max_threads(""));
  EXPECT_EQ(std::nullopt, redex_parallel::impl::parse_max_threads("+36"));
  EXPECT_EQ(std::nullopt, redex_parallel::impl::parse_max_threads(" 36 "));
  EXPECT_EQ(std::nullopt, redex_parallel::impl::parse_max_threads("0"));
  EXPECT_EQ(std::nullopt, redex_parallel::impl::parse_max_threads("-1"));
  EXPECT_EQ(std::nullopt, redex_parallel::impl::parse_max_threads("invalid"));
  EXPECT_EQ(std::nullopt,
            redex_parallel::impl::parse_max_threads("36 trailing"));
}

TEST(WorkQueueDeathTest, DefaultNumThreadsReadsEnvironmentCap) {
#if defined(__linux__) && !defined(__ANDROID__)
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  ASSERT_EQ(0, sched_getaffinity(0, sizeof(cpus), &cpus));
  if (CPU_COUNT(&cpus) <= 1) {
    GTEST_SKIP() << "The cap cannot narrow a one-CPU affinity mask";
  }

  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      {
        if (setenv("REDEX_MAX_THREADS", "1", /* overwrite */ 1) != 0) {
          std::_Exit(2);
        }
        std::_Exit(redex_parallel::default_num_threads() == 1 ? 0 : 1);
      },
      ::testing::ExitedWithCode(0), "");
#endif
}

TEST(WorkQueueDeathTest, DefaultNumThreadsRejectsInvalidEnvironmentCap) {
#if defined(__linux__) && !defined(__ANDROID__)
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(
      {
        if (setenv("REDEX_MAX_THREADS", "invalid", /* overwrite */ 1) != 0) {
          std::_Exit(2);
        }
        try {
          (void)redex_parallel::default_num_threads();
        } catch (const RedexException&) {
          std::_Exit(0);
        }
        std::_Exit(1);
      },
      ::testing::ExitedWithCode(0), "");
#endif
}

//==========
// Test for correctness. Duplicate of `sparta::WorkQueue` to check that the
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
      [&](sparta::WorkerState<int>* worker_state, int a) {
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
