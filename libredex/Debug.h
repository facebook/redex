/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexException.h"

#include <iosfwd>
#include <stdexcept>

constexpr bool debug =
#ifdef NDEBUG
    false
#else
    true
#endif // NDEBUG
    ;

extern bool slow_invariants_debug;

#ifdef _MSC_VER
#define DEBUG_ONLY

#define not_reached()    \
  do {                   \
    redex_assert(false); \
    __assume(false);     \
  } while (true)
#define not_reached_log(msg, ...)          \
  do {                                     \
    assert_log(false, msg, ##__VA_ARGS__); \
    __assume(false);                       \
  } while (true)

#define assert_fail_impl(e, type, msg, ...) \
  assert_fail(#e, __FILE__, __LINE__, __func__, type, msg, ##__VA_ARGS__)
#else
#define DEBUG_ONLY __attribute__((unused))

#define not_reached()        \
  do {                       \
    redex_assert(false);     \
    __builtin_unreachable(); \
  } while (true)
#define not_reached_log(msg, ...)          \
  do {                                     \
    assert_log(false, msg, ##__VA_ARGS__); \
    __builtin_unreachable();               \
  } while (true)

#define assert_fail_impl(e, type, msg, ...) \
  assert_fail(                              \
      #e, __FILE__, __LINE__, __PRETTY_FUNCTION__, type, msg, ##__VA_ARGS__)
#endif

[[noreturn]] void assert_fail(const char* expr,
                              const char* file,
                              unsigned line,
                              const char* func,
                              RedexError type,
                              const char* fmt,
                              ...);

#define assert_impl(cond, fail) \
  ((cond) ? static_cast<void>(0) : ((fail), static_cast<void>(0)))

#define always_assert(e) \
  assert_impl(e, assert_fail_impl(e, RedexError::GENERIC_ASSERTION_ERROR, ""))
#define always_assert_log(e, msg, ...) \
  assert_impl(e,                       \
              assert_fail_impl(        \
                  e, RedexError::GENERIC_ASSERTION_ERROR, msg, ##__VA_ARGS__))
#define always_assert_type_log(e, type, msg, ...) \
  assert_impl(e, assert_fail_impl(e, type, msg, ##__VA_ARGS__))
#undef assert

// A common definition for non-always asserts. Ensures that there won't be
// "-Wunused" warnings. The `!debug` should be optimized away since it is a
// constexpr.
#define redex_assert(e) always_assert(!debug || (e))
#define assert_log(e, msg, ...) \
  always_assert_log(!debug || e, msg, ##__VA_ARGS__)
#define assert_type_log(e, type, msg, ...) \
  always_assert_type_log(!debug || e, type, msg, ##__VA_ARGS__)

void print_stack_trace(std::ostream& os, const std::exception& e);

void crash_backtrace_handler(int sig);

// If `block` is true, only a single assert will be logged. All following
// asserts will sleep forever.
void block_multi_asserts(bool block);

// If called, assertions on threads other than the caller may immediately abort
// instead of raising an exception. Currently only implemented for Linux.
// Note: this is a workaround for libstdc++ from GCC < 8.
void set_abort_if_not_this_thread();

// Stats from /proc. See http://man7.org/linux/man-pages/man5/proc.5.html.
struct VmStats {
  uint64_t vm_peak = 0; // "Peak virtual memory size."
  uint64_t vm_hwm = 0; // "Peak resident set size ("high water mark")."
};
VmStats get_mem_stats();
bool try_reset_hwm_mem_stat(); // Attempt to reset the vm_hwm value.
