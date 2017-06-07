/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <functional>
#include <string>
#include <chrono>

struct Timer {
  Timer(std::string msg);
  Timer(std::string msg, std::function<void(double)> on_destruct);
  ~Timer();

 private:
  static unsigned s_indent;
  std::string m_msg;
  std::function<void(double)> m_on_destruct;
  std::chrono::high_resolution_clock::time_point m_start;
};
