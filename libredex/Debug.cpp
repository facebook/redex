/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Debug.h"

#include <exception>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _MSC_VER
#include <execinfo.h>
#include <unistd.h>
#endif

namespace {
void crash_backtrace() {
#ifndef _MSC_VER
  constexpr int max_bt_frames = 256;
  void* buf[max_bt_frames];
  auto frames = backtrace(buf, max_bt_frames);
  backtrace_symbols_fd(buf, frames, STDERR_FILENO);
#endif
}
}; // namespace

void crash_backtrace_handler(int sig) {
  crash_backtrace();

  signal(sig, SIG_DFL);
  raise(sig);
}

void assert_fail(const char* expr,
                 const char* file,
                 unsigned line,
                 const char* func,
                 const char* fmt,
                 ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(
      stderr, "%s:%u: %s: assertion `%s' failed.\n", file, line, func, expr);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  // our signal handlers will call this too, but they won't print the full
  // stack if the exception has been rethrown
  crash_backtrace();
  throw std::runtime_error("Redex assertion failure");
}
