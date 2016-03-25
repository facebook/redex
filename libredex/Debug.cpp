/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Debug.h"

#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
  abort();
}

void crash_backtrace(int sig) {
  constexpr int max_bt_frames = 256;
  void* buf[max_bt_frames];
  auto frames = backtrace(buf, max_bt_frames);
  backtrace_symbols_fd(buf, frames, STDERR_FILENO);

  signal(sig, SIG_DFL);
  raise(sig);
}
