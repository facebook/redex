/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Debug.h"
#include "DebugUtils.h"

#include <array>
#include <atomic>
#include <cstring>
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

#include "Macros.h"
#include "Trace.h"
#include "TraceContextAccess.h"

#if !IS_WINDOWS
#include <execinfo.h>
#include <unistd.h>
#elif defined(_MSC_VER)
// Need sleep.
#include <windows.h>
#define sleep(x) Sleep(1000 * (x))
#else
// Mingw has unistd with sleep.
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <boost/algorithm/string.hpp>
#include <boost/exception/all.hpp>
#ifdef __APPLE__
#define BOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED
#endif
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include <boost/stacktrace.hpp>
#pragma GCC diagnostic pop

// By default, run with slow invariant checks in debug mode.
bool slow_invariants_debug{debug};

namespace {

// This is a macro to avoid extra frames for symbolization.
#if !IS_WINDOWS
#define CRASH_BACKTRACE()                             \
  do {                                                \
    constexpr int max_bt_frames = 256;                \
    void* buf[max_bt_frames];                         \
    auto frames = backtrace(buf, max_bt_frames);      \
    backtrace_symbols_fd(buf, frames, STDERR_FILENO); \
  } while (0)
#else
#define CRASH_BACKTRACE()
#endif

std::atomic<size_t> g_crashing{0};

}; // namespace

namespace redex_debug {

namespace {

std::array<bool, RedexError::MAX + 1> abort_for_type{};
std::array<bool, RedexError::MAX + 1> no_stacktrace_for_type{};

} // namespace

void set_exc_type_as_abort(RedexError type) { abort_for_type[type] = true; }
void disable_stack_trace_for_exc_type(RedexError type) {
  no_stacktrace_for_type[type] = true;
}

} // namespace redex_debug

void crash_backtrace_handler(int sig) {
  size_t crashing = g_crashing.fetch_add(1);
  if (crashing == 0) {
    CRASH_BACKTRACE();
  } else {
    sleep(60); // Sleep a minute, then go on to die if we're still alive.
  }

  signal(sig, SIG_DFL);
  raise(sig);
}

// Exists purely so that the type of crash can be distinguished in the wrapper.
void debug_backtrace_handler(int sig) {
  size_t crashing = g_crashing.fetch_add(1);
  if (crashing == 0) {
    CRASH_BACKTRACE();
  } else {
    sleep(60); // Sleep a minute, then go on to die if we're still alive.
  }

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

#if !IS_WINDOWS

// To have unified decoding in redex.py, use GNU backtrace here with a helper.
struct StackTrace {
  std::array<void*, 256> trace;
  size_t len;

  StackTrace() { len = backtrace(trace.data(), trace.size()); }

  void print_to_stderr() const {
    backtrace_symbols_fd(trace.data(), len, STDERR_FILENO);
  }
};

using StType = StackTrace;

void print_stack_trace_impl(std::ostream& /* os */, const StackTrace* st) {
  // Does not support ostream right now.
  st->print_to_stderr();
}

#else

// Use boost stacktrace on Windows.
using StType = boost::stacktrace::stacktrace;

void print_stack_trace_impl(std::ostream& os, const StType* st) {
  os << *st << std::endl;
}

#endif

using traced = boost::error_info<struct tag_stacktrace, StType>;

#ifdef __linux__
std::atomic<pid_t> g_aborting{0};
pid_t get_tid() { return syscall(SYS_gettid); }
pid_t g_abort_if_not_tid{0};
#endif
bool g_block_multi_asserts{false};

} // namespace

void block_multi_asserts(bool block) {
  g_block_multi_asserts = block; // Ignore races and such.
}

void set_abort_if_not_this_thread() {
#if defined(__linux__)
  g_aborting = 0; // Reset for good measure.
#endif
#if defined(__linux__) && defined(__GNUC__)
// See https://gcc.gnu.org/bugzilla/show_bug.cgi?id=55917.
#if __GNUC__ < 8
  g_abort_if_not_tid = get_tid();
#endif
#endif
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
  auto context_str = []() {
    std::string res;
#if !IS_WINDOWS
    auto* trace_context = TraceContextAccess::get_s_context();
    if (trace_context != nullptr) {
      res += " (Context: ";
      res += trace_context->get_string_value();
      res += ")";
    }
#endif
    return res;
  };
  std::string msg = format2string("%s:%u: %s: assertion `%s' failed.%s\n", file,
                                  line, func, expr, context_str().c_str());

  if (strcmp(fmt, " ") != 0) {
    msg += v_format2string(fmt, ap);
  }

  va_end(ap);

  bool do_throw;
#ifdef __linux__
  pid_t cur = get_tid();
  pid_t expected = 0;
  do_throw = g_aborting.compare_exchange_strong(expected, cur);
  if (!do_throw) {
    do_throw = expected == cur;
  }
#else
  do_throw = true;
#endif

  if (!do_throw && g_block_multi_asserts) {
    // Another thread already threw. Avoid "terminate called recursively."
    // Infinite loop.
    for (;;) {
      sleep(1000);
    }
  }

  if (redex_debug::abort_for_type[type]) {
    // Pretend a termination for `redex.py`.
    std::cerr << "terminate called after assertion" << std::endl;
    std::cerr << "  what():  RedexError: " << type << " " << msg << std::endl;
    if (!redex_debug::no_stacktrace_for_type[type]) {
      CRASH_BACKTRACE();
    }
    _exit(-6);
  }

#ifdef __linux__
  // Asked to abort if not the set thread. Print message and exit.
  if (g_abort_if_not_tid != 0 && g_abort_if_not_tid != cur) {
    // Pretend a termination for `redex.py`.
    std::cerr << "terminate called after assertion" << std::endl;
    std::cerr << "  what():  RedexError: " << type << " " << msg << std::endl;
    abort();
  }
#endif

  throw boost::enable_error_info(RedexException(type, msg)) << traced(StType());
}

void print_stack_trace(std::ostream& os, const std::exception& e) {
  const StType* st = boost::get_error_info<traced>(e);
  if (st) {
    print_stack_trace_impl(os, st);
  }
}

VmStats get_mem_stats() {
  VmStats res;
  std::ifstream ifs("/proc/self/status");
  if (ifs.fail()) {
    return res;
  }
  const std::array<std::pair<const char*, uint64_t*>, 3> relevant_stats = {{
      {"VmPeak:", &res.vm_peak},
      {"VmHWM:", &res.vm_hwm},
      {"VmRSS:", &res.vm_rss},
  }};

  std::string line;
  std::regex re("[^:]*:\\s*([0-9]*)\\s*(.)B");
  while (std::getline(ifs, line)) {
    auto it_relevant_stat = std::find_if(
        relevant_stats.begin(), relevant_stats.end(),
        [&line](const auto& rs) { return boost::starts_with(line, rs.first); });

    if (it_relevant_stat != relevant_stats.end()) {
      auto& [stat_name, stat_field_ptr] = *it_relevant_stat;

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

      *stat_field_ptr = val;
      if (std::all_of(relevant_stats.begin(), relevant_stats.end(),
                      [](const auto& rs) { return *rs.second != 0; })) {
        break;
      }
    }
  }
  return res;
}

bool try_reset_hwm_mem_stat() {
  // See http://man7.org/linux/man-pages/man5/proc.5.html for `clear_refs` and
  // the magic `5`.
  std::ofstream ifs("/proc/self/clear_refs");
  if (ifs.fail()) {
    return false;
  }
  ifs << 5;
  return true;
}
