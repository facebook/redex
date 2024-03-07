/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct Timer {
  explicit Timer(const std::string& msg, bool indent = true);
  ~Timer();

  using times_t = std::vector<std::pair<std::string, double>>;

  // there should be no currently running Timers when this function is called
  static const times_t& get_times() { return s_times; }

  static void add_timer(std::string msg, double dur_s);

 private:
  static std::mutex s_lock;
  static times_t s_times;
  static unsigned s_indent;
  std::string m_msg;
  std::chrono::high_resolution_clock::time_point m_start;
  bool m_indent;
};

// An accumulating thread-safe timer with a scope-based approach.
// Note: uses uint64_t microseconds to simplify and use optimized atomic.
class AccumulatingTimer {
 public:
  class AccumulatingTimerScope {
   public:
    explicit AccumulatingTimerScope(AccumulatingTimer* context)
        : m_context(context),
          m_start(std::chrono::high_resolution_clock::now()) {}
    ~AccumulatingTimerScope() {
      auto end = std::chrono::high_resolution_clock::now();
      auto dur_in_mus =
          std::chrono::duration_cast<std::chrono::microseconds>(end - m_start);
      m_context->m_microseconds->fetch_add((uint64_t)dur_in_mus.count());
    }

    // Disallow copying.
    AccumulatingTimerScope(const AccumulatingTimerScope&) = delete;
    AccumulatingTimerScope& operator=(const AccumulatingTimerScope&) = delete;

    // Allow move.
    AccumulatingTimerScope(AccumulatingTimerScope&&) = default;
    AccumulatingTimerScope& operator=(AccumulatingTimerScope&&) = default;

   private:
    AccumulatingTimer* m_context;
    std::chrono::high_resolution_clock::time_point m_start;
  };

  AccumulatingTimer() {}

  explicit AccumulatingTimer(std::string msg);

  AccumulatingTimerScope scope() { return AccumulatingTimerScope(this); }

  uint64_t get_microseconds() const { return m_microseconds->load(); }
  double get_seconds() const {
    return ((double)m_microseconds->load()) / 1000000;
  }

  using times_t = std::vector<std::pair<std::string, double>>;

  // there should be no currently running Timers when this function is called
  static times_t get_times();

  static void add_timer(std::string msg,
                        std::shared_ptr<std::atomic<uint64_t>> microseconds);

 private:
  using times_impl_t =
      std::list<std::pair<std::string, std::shared_ptr<std::atomic<uint64_t>>>>;

  static std::mutex s_lock;
  // We dynamically allocate the static list of times to avoid problems with the
  // static initialization / destruction order.
  static times_impl_t* s_times;
  std::shared_ptr<std::atomic<uint64_t>> m_microseconds{
      std::make_shared<std::atomic<uint64_t>>(0)};
};
