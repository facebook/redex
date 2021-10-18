/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
  // Instance method foo should be a static method which takes an A as the only
  // arg.
  auto foo = find_dmethod_named(*a, "foo");
  EXPECT_NE(foo, nullptr);
  auto args = foo->get_proto()->get_args();
  EXPECT_EQ(args->size(), 1);
  EXPECT_STREQ(args->at(0)->c_str(), CLASS_A);
  EXPECT_TRUE(is_static(foo));

  auto baz = find_dmethod_named(*a, "baz");
  EXPECT_NE(baz, nullptr);
  EXPECT_TRUE(is_static(baz));

  auto b = find_class_named(classes, CLASS_B);
  auto bar = find_dmethod_named(*b, "bar");
  EXPECT_NE(bar, nullptr);
  // Same as foo, should now have 1 argument, which previously was the "this"
  // object
  auto b_args = bar->get_proto()->get_args();
  EXPECT_EQ(b_args->size(), 1);
  EXPECT_STREQ(b_args->at(0)->c_str(), CLASS_B);
  EXPECT_TRUE(is_static(bar));
}
