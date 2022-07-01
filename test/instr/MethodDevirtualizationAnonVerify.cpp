/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VerifyUtil.h"
#include <gtest/gtest.h>

const char* const CLASS_A = "Lcom/facebook/redextest/A;";
const char* const CLASS_B = "Lcom/facebook/redextest/B;";

TEST_F(PostVerify, MethodStatic) {
  auto a = find_class_named(classes, CLASS_A);
  auto foo = find_dmethod_named(*a, "foo");
  EXPECT_NE(foo, nullptr);

  auto baz = find_dmethod_named(*a, "baz");
  EXPECT_NE(baz, nullptr);

  auto b = find_class_named(classes, CLASS_B);
  auto foo2 = find_dmethod_named(*b, "foo");
  EXPECT_EQ(foo2, nullptr);

  auto baz2 = find_dmethod_named(*b, "baz");
  EXPECT_EQ(baz2, nullptr);
}
