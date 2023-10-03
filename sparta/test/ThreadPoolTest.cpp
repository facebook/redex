/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <sparta/ThreadPool.h>

#include <array>
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <random>

constexpr unsigned int NUM = 1000;

//==========
// Test for correctness
//==========

TEST(ThreadPoolTest, AsyncRunner) {
  sparta::ThreadPool<> thread_pool;
  sparta::AsyncRunner* async_runner = &thread_pool;
  std::condition_variable condition_variable;
  std::mutex mutex;
  size_t counter{0};
  auto f = [&](size_t) {
    std::lock_guard<std::mutex> lock_guard(mutex);
    if (++counter == NUM) {
      condition_variable.notify_one();
    }
  };
  for (size_t i = 0; i < NUM; i++) {
    async_runner->run_async(f, i);
  }
  std::unique_lock<std::mutex> unique_lock(mutex);
  condition_variable.wait(unique_lock, [&]() { return counter == NUM; });
}

TEST(ThreadPoolTest, ThreadPool) {
  sparta::ThreadPool<> thread_pool;
  for (size_t rounds = 0; rounds < 10; rounds++) {
    std::condition_variable condition_variable;
    std::mutex mutex;
    bool blocked{true};
    auto f = [&](size_t) {
      std::unique_lock<std::mutex> unique_lock(mutex);
      condition_variable.wait(unique_lock, [&]() { return !blocked; });
    };
    for (size_t i = 0; i < NUM; i++) {
      thread_pool.run_async(f, i);
    }
    ASSERT_EQ(NUM, thread_pool.size());

    {
      std::lock_guard<std::mutex> lock_guard(mutex);
      blocked = false;
    }
    condition_variable.notify_all();

    thread_pool.join();
    ASSERT_TRUE(thread_pool.empty());
  }
}

TEST(ThreadPoolTest, exceptionPropagation) {
  sparta::ThreadPool<> thread_pool;
  thread_pool.run_async([]() { throw std::logic_error("exception!"); });

  ASSERT_THROW(thread_pool.join(), std::logic_error);
}

TEST(ThreadPoolTest, multipleExceptions) {
  sparta::ThreadPool<> thread_pool;

  for (int idx = 0; idx < NUM; ++idx) {
    thread_pool.run_async([]() { throw std::logic_error("exception!"); });
  }

  ASSERT_THROW(thread_pool.join(), std::logic_error);
}
