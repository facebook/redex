/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Timer.h"

#include "Trace.h"

unsigned Timer::s_indent = 0;
std::mutex Timer::s_lock;
Timer::times_t Timer::s_times;

Timer::Timer(const std::string& msg)
    : m_msg(msg), m_start(std::chrono::high_resolution_clock::now()) {
  ++s_indent;
}

Timer::~Timer() {
  --s_indent;
  auto end = std::chrono::high_resolution_clock::now();
  auto duration_s = std::chrono::duration<double>(end - m_start).count();
  TRACE(TIME, 1, "%*s%s completed in %.1lf seconds", 4 * s_indent, "",
        m_msg.c_str(), duration_s);

  Timer::add_timer(std::move(m_msg), duration_s);
}

void Timer::add_timer(std::string&& msg, double dur_s) {
  std::lock_guard<std::mutex> guard(s_lock);
  s_times.emplace_back(std::move(msg), dur_s);
}
