/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <stdexcept>

constexpr bool debug =
#ifdef NDEBUG
    false
#else
    true
#endif // NDEBUG
    ;

#ifdef _MSC_VER
#define DEBUG_ONLY

#define not_reached() \
  do {                \
    assert(false);    \
    __assume(false);  \
  } while (true)

#define assert_fail_impl(e, msg, ...) \
  assert_fail(#e, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#else
#define DEBUG_ONLY __attribute__((unused))

#define not_reached()        \
  do {                       \
    assert(false);           \
    __builtin_unreachable(); \
  } while (true)

#define assert_fail_impl(e, msg, ...) \
  assert_fail(#e, __FILE__, __LINE__, __PRETTY_FUNCTION__, msg, ##__VA_ARGS__)
#endif

[[noreturn]] void assert_fail(const char* expr,
                              const char* file,
                              unsigned line,
                              const char* func,
                              const char* fmt,
                              ...);

#define assert_impl(cond, fail) \
  ((cond) ? static_cast<void>(0) : ((fail), static_cast<void>(0)))

#define always_assert(e) assert_impl(e, assert_fail_impl(e, ""))
#define always_assert_log(e, msg, ...) \
  assert_impl(e, assert_fail_impl(e, msg, ##__VA_ARGS__))

#undef assert

#ifdef NDEBUG
#define assert(e) static_cast<void>(0)
#define assert_log(e, msg, ...) static_cast<void>(0)
#else
#define assert(e) always_assert(e)
#define assert_log(e, msg, ...) always_assert_log(e, msg, ##__VA_ARGS__)
#endif // NDEBUG

void crash_backtrace_handler(int sig);
