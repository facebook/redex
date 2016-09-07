/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Timer.h"

#include "Trace.h"

unsigned Timer::s_indent = 0;

Timer::Timer(std::string msg)
  : m_msg(msg),
    m_start(std::chrono::high_resolution_clock::now())
{
  ++s_indent;
}

Timer::~Timer() {
  --s_indent;
  auto end = std::chrono::high_resolution_clock::now();
  TRACE(TIME, 1, "%*s%s completed in %.1lf seconds\n",
        4 * s_indent, "",
        m_msg.c_str(),
        std::chrono::duration<double>(end - m_start).count());
}
