/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>
#include "VerifyUtil.h"

namespace {

void assert_method(DexMethod* m, const char* method_name) {
  // Seems like this can only work in a void method.
  ASSERT_NE(m, nullptr) << "Did not find method " << method_name;
}

DexMethod* find_method_assert(const DexClass* cls, const char* method_name) {
  auto m = find_method_named(*cls, method_name);
  assert_method(m, method_name);
  return m;
}

} // namespace

TEST_F(PostVerify, ClassFinal) {
  auto super_cls = find_class_named(classes, "Lredex/Super;");
  EXPECT_FALSE(is_final(super_cls));
  auto sub = find_class_named(classes, "Lredex/Sub;");
  EXPECT_TRUE(is_final(sub));
}

TEST_F(PostVerify, ClassAbstract) {
  auto cls = find_class_named(classes, "Lredex/Abstract;");
  EXPECT_FALSE(is_final(cls));
}

TEST_F(PostVerify, MethodFinal) {
  auto super_cls = find_class_named(classes, "Lredex/Super;");
  auto super_foo = find_method_assert(super_cls, "foo");
  EXPECT_FALSE(is_final(super_foo));

  auto sub = find_class_named(classes, "Lredex/Sub;");
  auto sub_foo = find_method_assert(sub, "foo");
  EXPECT_TRUE(is_final(sub_foo));
}

TEST_F(PostVerify, MethodStatic) {
  auto super_cls = find_class_named(classes, "Lredex/Super;");
  auto bar = find_method_assert(super_cls, "bar");
  EXPECT_TRUE(is_final(bar));

  auto sub = find_class_named(classes, "Lredex/Sub;");
  auto baz = find_method_assert(sub, "baz");
  EXPECT_TRUE(is_final(baz));
}

TEST_F(PostVerify, MethodAbstract) {
  auto cls = find_class_named(classes, "Lredex/Abstract;");
  auto nope = find_method_assert(cls, "nope");
  EXPECT_FALSE(is_static(nope));
  EXPECT_FALSE(is_final(nope));
}

TEST_F(PostVerify, MethodPrivate) {
  auto cls = find_class_named(classes, "Lredex/Doubler;");
  auto doubleit = find_method_assert(cls, "doubleit");
  EXPECT_TRUE(is_private(doubleit));
}
