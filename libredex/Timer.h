/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct Timer {
  using time_point = std::chrono::high_resolution_clock::time_point;

  struct Event {
    std::string name;
    uint16_t depth;
    time_point start;
    time_point end;
    Event(std::string name,
          uint16_t depth,
          time_point start,
          time_point end)
        : name(std::move(name)), depth(depth), start(start), end(end) {}
  };

  Timer(const std::string& msg);

  ~Timer();

  using times_t = std::vector<Event>;
  // there should be no currently running Timers when this function is called
  static const times_t& get_times() { return s_times; }

 private:
  static std::mutex s_lock;
  static times_t s_times;
  static uint16_t s_indent;
  std::string m_msg;
  std::chrono::high_resolution_clock::time_point m_start;
};
