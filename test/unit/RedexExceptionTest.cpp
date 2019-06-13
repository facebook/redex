/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Debug.h"

void old_assert() { always_assert_log(1 == 2, "what? %d != %d?", 1, 2); }

void typed_assert() {
  always_assert_type_log(1 == 2, RedexError::INTERNAL_ERROR, "what? %d != %d?",
                         1, 2);
}

// the following test will fail if line number of always_assert_log is changed.
TEST(RedexException, test_assert_log) {
  try {
    old_assert();
  } catch (const RedexException& e) {
    EXPECT_EQ(RedexError::GENERIC_ASSERTION_ERROR, e.type);
    std::string expected_msg =
        "void old_assert(): assertion `1 == 2' failed.\nwhat? 1 != 2?";
    EXPECT_NE(std::string(e.what()).find(expected_msg), std::string::npos);
  }
}

// the following test will fail if line number of always_assert_log is changed.
TEST(RedexException, test_assert_type_log) {
  try {
    typed_assert();
  } catch (const RedexException& e) {
    EXPECT_EQ(RedexError::INTERNAL_ERROR, e.type);
    std::string expected_msg =
        "void typed_assert(): assertion `1 == 2' failed.\nwhat? 1 != 2?";
    EXPECT_NE(std::string(e.what()).find(expected_msg), std::string::npos);
  }
}
