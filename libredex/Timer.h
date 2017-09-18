/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct Timer {
  Timer(const std::string& msg);
  ~Timer();

  using times_t = std::vector<std::pair<std::string, double>>;
  // there should be no currently running Timers when this function is called
  static const times_t& get_times() {
    return s_times;
  }

 private:
  static std::mutex s_lock;
  static times_t s_times;
  static unsigned s_indent;
  std::string m_msg;
  std::chrono::high_resolution_clock::time_point m_start;
};
