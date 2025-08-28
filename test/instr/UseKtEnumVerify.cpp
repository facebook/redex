/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VerifyUtil.h"
#include <gtest/gtest.h>

TEST_F(PostVerify, EnumShouldStillExist) {
  std::vector<std::string> enum_names = {"Lredex/D;", "Lredex/F;"};
  for (auto& enum_name : enum_names) {
    auto* cls = find_class_named(classes, enum_name.c_str());
    EXPECT_NE(cls, nullptr) << enum_name << " should still exist!";
    // OptimizeEnumsPass seems to clear the flag on the class, and hangs various
    // helper methods on it. Verify that it looks untouched.
    EXPECT_TRUE(is_enum(cls))
        << enum_name << " should not have been optimized!";
  }
}
