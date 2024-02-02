/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <exception>
#include <gtest/gtest.h>

#include "Debug.h"
#include "DexClass.h"
#include "RedexTest.h"
#include "Trace.h"

class AssertTest : public RedexTest {};

TEST_F(AssertTest, AssertPass) { assert_log(true, "Test message"); }

TEST_F(AssertTest, AssertMaybeThrow) {
#ifdef NDEBUG
  constexpr bool kDebug = false;
#else
  constexpr bool kDebug = true;
#endif

  try {
    assert_log(false, "Test message");
    EXPECT_TRUE(!kDebug) << "Expected exception not thrown";
  } catch (std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("Test message"), std::string::npos)
        << e.what();
  }
}

TEST_F(AssertTest, AlwaysAssertPass) {
  always_assert_log(true, "Test message");
}

TEST_F(AssertTest, AlwaysAssertThrow) {
  try {
    always_assert_log(false, "Test message");
    EXPECT_TRUE(false) << "Expected exception not thrown";
  } catch (std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("Test message"), std::string::npos)
        << e.what();
  }
}

TEST_F(AssertTest, AlwaysAssertTraceContextStringThrow) {
  std::string a_string{"A string"};
  TraceContext context{&a_string};
  try {
    always_assert_log(false, "Test message");
    EXPECT_TRUE(false) << "Expected exception not thrown";
  } catch (std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("Test message"), std::string::npos)
        << e.what();
    EXPECT_NE(std::string(e.what()).find("(Context: A string)"),
              std::string::npos)
        << e.what();
  }
}

TEST_F(AssertTest, AlwaysAssertTraceContextDexTypeThrow) {
  auto* a_type = DexType::make_type("LA;");
  TraceContext context{a_type};
  try {
    always_assert_log(false, "Test message");
    EXPECT_TRUE(false) << "Expected exception not thrown";
  } catch (std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("Test message"), std::string::npos)
        << e.what();
    EXPECT_NE(std::string(e.what()).find("(Context: LA;)"), std::string::npos)
        << e.what();
  }
}

TEST_F(AssertTest, AlwaysAssertTraceContextDexMethodThrow) {
  auto* mref = DexMethod::make_method("LFoo;.m:()V");
  TraceContext context{mref};
  try {
    always_assert_log(false, "Test message");
    EXPECT_TRUE(false) << "Expected exception not thrown";
  } catch (std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("Test message"), std::string::npos)
        << e.what();
    EXPECT_NE(std::string(e.what()).find("(Context: LFoo;.m:()V)"),
              std::string::npos)
        << e.what();
  }
}
