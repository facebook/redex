/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Macros.h" // For ATTR_FORMAT.
#include "RedexException.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

constexpr bool debug =
#ifdef NDEBUG
    false
#else
    true
#endif // NDEBUG
    ;

extern bool slow_invariants_debug;

#define DEBUG_ONLY [[maybe_unused]]

#ifdef _MSC_VER
#define UNREACHABLE() __assume(false)
#define PRETTY_FUNC() __func__
#else
#define UNREACHABLE() __builtin_unreachable()
#define PRETTY_FUNC() __PRETTY_FUNCTION__
#endif // _MSC_VER

#define not_reached()    \
  do {                   \
    redex_assert(false); \
    UNREACHABLE();       \
  } while (true)
#define not_reached_log(msg, ...)          \
  do {                                     \
    assert_log(false, msg, ##__VA_ARGS__); \
    UNREACHABLE();                         \
  } while (true)

#define assert_fail_impl(e, type, msg, ...) \
  assert_fail(#e, __FILE__, __LINE__, PRETTY_FUNC(), type, msg, ##__VA_ARGS__)

[[noreturn]] void assert_fail(const char* expr,
                              const char* file,
                              unsigned line,
                              const char* func,
                              RedexError type,
                              const char* fmt,
                              ...) ATTR_FORMAT(6, 7);

#define assert_impl(cond, fail) \
  ((cond) ? static_cast<void>(0) : ((fail), static_cast<void>(0)))

// Note: Using " " and detecting that, so that GCC's `-Wformat-zero-length`
//       does not apply, which is hard to disable across compilers.
#define always_assert(e) \
  assert_impl(e, assert_fail_impl(e, RedexError::GENERIC_ASSERTION_ERROR, " "))
#define always_assert_log(e, msg, ...) \
  assert_impl(e,                       \
              assert_fail_impl(        \
                  e, RedexError::GENERIC_ASSERTION_ERROR, msg, ##__VA_ARGS__))
#define always_assert_type_log(e, type, msg, ...) \
  assert_impl(e, assert_fail_impl(e, type, msg, ##__VA_ARGS__))

// A common definition for non-always asserts. Ensures that there won't be
// "-Wunused" warnings. The `!debug` should be optimized away since it is a
// constexpr.
#define redex_assert(e) always_assert(!debug || (e))
#define assert_log(e, msg, ...) \
  always_assert_log(!debug || e, msg, ##__VA_ARGS__)
#define assert_type_log(e, type, msg, ...) \
  always_assert_type_log(!debug || e, type, msg, ##__VA_ARGS__)

// Helper for const assertions.
#define CONSTP(e)                                               \
  static_cast<std::add_pointer_t<                               \
      typename std::add_const_t<typename std::remove_pointer_t< \
          typename std::remove_reference_t<decltype(e)>>>>>(e)

namespace redex {

void set_throw_typed_exception(bool throw_typed);
bool throw_typed_exception();

struct DexAssert {
  static void always(bool cond, const std::string& msg) {
    always_assert_type_log(cond, RedexError::INVALID_DEX, "%s", msg.c_str());
  }

  static void always(bool cond, const std::string_view& msg) {
    always_assert_type_log(
        cond, RedexError::INVALID_DEX, "%s", std::string(msg).c_str());
  }

  static void always(bool cond, const char* msg) {
    always_assert_type_log(cond, RedexError::INVALID_DEX, "%s", msg);
  }
};

} // namespace redex
