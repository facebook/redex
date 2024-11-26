/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CppUtil.h"
#include "Debug.h"
#include "RedexTest.h"

class DebugTest : public RedexTest {};

TEST_F(DebugTest, slow_invariants_on_for_gtest) {
#ifdef NDEBUG
  constexpr bool kNDebug = true;
#else
  constexpr bool kNDebug = false;
#endif
  EXPECT_TRUE(slow_invariants_debug || kNDebug);
}

TEST_F(DebugTest, UntypedExceptions) {
  auto old_val = redex::throw_typed_exception();
  ScopeGuard sg{[&]() { redex::set_throw_typed_exception(old_val); }};
  redex::set_throw_typed_exception(false);

  EXPECT_THROW({ always_assert(false); }, RedexException);

  EXPECT_THROW(
      { always_assert_type_log(false, INVALID_DEX, "test"); }, RedexException);
  try {
    always_assert_type_log(false, INVALID_DEX, "test");
  } catch (const redex::InvalidDexException&) {
    EXPECT_TRUE(false) << "Got InvalidDexException";
  } catch (const RedexException&) {
  }
}

// This cannot be run in parallel in the same process.
TEST_F(DebugTest, TypedExceptions) {
  auto old_val = redex::throw_typed_exception();
  ScopeGuard sg{[&]() { redex::set_throw_typed_exception(old_val); }};
  redex::set_throw_typed_exception(true);

  EXPECT_THROW({ always_assert(false); }, RedexException);
  EXPECT_THROW(
      { always_assert_type_log(false, INVALID_DEX, "test"); },
      redex::InvalidDexException);
}
