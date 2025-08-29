/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WorkQueue.h"

#include <chrono>
#include <thread>

//==========
// Test for performance
//==========

template <typename T>
double calculate_speedup(std::vector<int>& wait_times, int num_threads) {
  auto wq = workqueue_foreach<int>(
      [](int a) { std::this_thread::sleep_for(T(a)); }, num_threads);

  for (auto& item : wait_times) {
    wq.add_item(item);
  }

  auto single_start = std::chrono::high_resolution_clock::now();
  for (auto& item : wait_times) {
    std::this_thread::sleep_for(T(item));
  }
  auto single_end = std::chrono::high_resolution_clock::now();

  auto para_start = std::chrono::high_resolution_clock::now();
  wq.run_all();
  auto para_end = std::chrono::high_resolution_clock::now();

  double duration1 =
      std::chrono::duration_cast<T>(single_end - single_start).count();
  double duration2 =
      std::chrono::duration_cast<T>(para_end - para_start).count();
  double speedup = duration1 / duration2;
  return speedup;
}

void profileBusyLoop() {
  std::vector<int> times;
  times.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    times.push_back(20);
  }
  double speedup = calculate_speedup<std::chrono::milliseconds>(
      times, std::thread::hardware_concurrency());
  printf("speedup busy loop: %f\n", speedup);
}

void variableLengthTasks() {
  std::vector<int> times;
  for (int i = 0; i < 50; ++i) {
    auto secs = rand() % 1000;
    times.push_back(secs);
  }
  double speedup = calculate_speedup<std::chrono::milliseconds>(times, 8);
  printf("speedup variable length tasks: %f\n", speedup);
}

void smallLengthTasks() {
  std::vector<int> times;
  times.reserve(1000);
  for (int i = 0; i < 1000; ++i) {
    times.push_back(10);
  }
  double speedup = calculate_speedup<std::chrono::microseconds>(times, 8);
  printf("speedup small length tasks: %f\n", speedup);
}

int main() {
  printf("Begin!\n");
  profileBusyLoop();
  variableLengthTasks();
  smallLengthTasks();
}
