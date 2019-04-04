/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "RedexException.h"
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

#define not_reached()    \
  do {                   \
    redex_assert(false); \
    __assume(false);     \
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

#ifdef NDEBUG
#define redex_assert(e) static_cast<void>(0)
#define assert_log(e, msg, ...) static_cast<void>(0)
#define assert_type_log(e, type, msg, ...) static_cast<void>(0)
#else
#define redex_assert(e) always_assert(e)
#define assert_log(e, msg, ...) always_assert_log(e, msg, ##__VA_ARGS__)
#define assert_type_log(e, type, msg, ...) \
  always_assert_type_log(e, type, msg, ##__VA_ARGS__)
#endif // NDEBUG

void crash_backtrace_handler(int sig);
