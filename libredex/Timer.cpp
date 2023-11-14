/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Timer.h"

#include <list>

#include "Trace.h"

unsigned Timer::s_indent = 0;
std::mutex Timer::s_lock;
Timer::times_t Timer::s_times;
std::mutex AccumulatingTimer::s_lock;
AccumulatingTimer::times_impl_t* AccumulatingTimer::s_times{nullptr};

Timer::Timer(const std::string& msg, bool indent)
    : m_msg(msg),
      m_start(std::chrono::high_resolution_clock::now()),
      m_indent(indent) {
  if (indent) {
    ++s_indent;
  }
}

Timer::~Timer() {
  if (m_indent) {
    --s_indent;
  }
  auto end = std::chrono::high_resolution_clock::now();
  auto duration_s = std::chrono::duration<double>(end - m_start).count();
  TRACE(TIME, 1, "%*s%s completed in %.1lf seconds", 4 * s_indent, "",
        m_msg.c_str(), duration_s);

  Timer::add_timer(std::move(m_msg), duration_s);
}

void Timer::add_timer(std::string msg, double dur_s) {
  std::lock_guard<std::mutex> guard(s_lock);
  s_times.emplace_back(std::move(msg), dur_s);
}

AccumulatingTimer::AccumulatingTimer(std::string msg) {
  add_timer(std::move(msg), m_microseconds);
}

AccumulatingTimer::times_t AccumulatingTimer::get_times() {
  std::lock_guard<std::mutex> guard(s_lock);
  AccumulatingTimer::times_t res;
  if (s_times) {
    res.reserve(s_times->size());
    for (auto& [str, microseconds] : *s_times) {
      res.emplace_back(str, ((double)microseconds->load()) / 1000000);
    }
  }
  return res;
}

void AccumulatingTimer::add_timer(
    std::string msg, std::shared_ptr<std::atomic<uint64_t>> microseconds) {
  std::lock_guard<std::mutex> guard(s_lock);
  if (!s_times) {
    s_times = new AccumulatingTimer::times_impl_t();
  }
  s_times->emplace_back(std::move(msg), std::move(microseconds));
}
