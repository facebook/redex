/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Debug.h"

#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <regex>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#ifndef _MSC_VER
#include <execinfo.h>
#include <unistd.h>
#endif

#include <boost/algorithm/string.hpp>
#include <boost/exception/all.hpp>
#ifdef __APPLE__
#define BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED
#endif
#include <boost/stacktrace.hpp>

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

namespace {

using traced =
    boost::error_info<struct tag_stacktrace, boost::stacktrace::stacktrace>;

} // namespace

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
      "%s:%u: %s: assertion `%s' failed.\n", file, line, func, expr);

  msg += v_format2string(fmt, ap);

  va_end(ap);
  throw boost::enable_error_info(RedexException(type, msg))
      << traced(boost::stacktrace::stacktrace());
}

void print_stack_trace(std::ostream& os, const std::exception& e) {
  const boost::stacktrace::stacktrace* st = boost::get_error_info<traced>(e);
  if (st) {
    os << *st << std::endl;
  }
}

VmStats get_mem_stats() {
  VmStats res;
  std::ifstream ifs("/proc/self/status");
  if (ifs.fail()) {
    return res;
  }
  std::string line;
  std::regex re("[^:]*:\\s*([0-9]*)\\s*(.)B");
  while (std::getline(ifs, line)) {
    bool is_vm_peak = boost::starts_with(line, "VmPeak:");
    bool is_vm_hwm = boost::starts_with(line, "VmHWM:");
    if (is_vm_peak || is_vm_hwm) {
      std::smatch match;
      bool matched = std::regex_match(line, match, re);
      if (!matched) {
        std::cerr << "Error: could not match " << line << std::endl;
        continue;
      }
      std::string num_str = match.str(1);
      std::string size_prefix_str = match.str(2);

      uint64_t val;
      try {
        size_t idx;
        val = std::stoull(num_str, &idx);
      } catch (...) {
        std::cerr << "Failed to parse numeric value in " << line << std::endl;
        continue;
      }

      if (size_prefix_str == "k" or size_prefix_str == "K") {
        val *= 1024;
      } else if (size_prefix_str == "M") {
        val *= 1024 * 1024;
      } else if (size_prefix_str == "G") {
        val *= 1024 * 1024 * 1024;
      } else {
        std::cerr << "Unknown size modifier in " << line << std::endl;
        continue;
      }

      if (is_vm_peak) {
        res.vm_peak = val;
      } else {
        res.vm_hwm = val;
      }
      if (res.vm_peak != 0 && res.vm_hwm != 0) {
        break;
      }
    }
  }
  return res;
}
