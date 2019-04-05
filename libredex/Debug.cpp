/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Debug.h"

#include <exception>
#include <memory>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

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

std::string v_format2string(const char* fmt, va_list ap) {
  va_list backup;
  va_copy(backup, ap);
  size_t size = vsnprintf(NULL, 0, fmt, ap);
  // size is the number of chars would had been written

  std::unique_ptr<char[]> buffer = std::make_unique<char[]>(size + 1);
  vsnprintf(buffer.get(), size + 1, fmt, backup);
  va_end(backup);
  std::string ret(buffer.get());
  return ret;
}

std::string format2string(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  auto ret = v_format2string(fmt, ap);
  va_end(ap);

  return ret;
}

void assert_fail(const char* expr,
                 const char* file,
                 unsigned line,
                 const char* func,
                 RedexError type,
                 const char* fmt,
                 ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string msg = format2string(
      "%s:%u: %s: assertion `%s' failed. ", file, line, func, expr);

  msg += v_format2string(fmt, ap);

  va_end(ap);
  // our signal handlers will call this too, but they won't print the full
  // stack if the exception has been rethrown
  crash_backtrace();
  throw RedexException(type, msg);
}
