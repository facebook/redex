/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "VerifyUtil.h"
#include <gtest/gtest.h>

TEST_F(PostVerify, FieldExistence) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redextest/RemoveUnusedFieldsTest;");
  ASSERT_NE(cls, nullptr);
  EXPECT_EQ(find_ifield_named(*cls, "unusedInt"), nullptr);
  // Removal of String is blacklisted in the test Redex config
  EXPECT_NE(find_ifield_named(*cls, "unusedString"), nullptr);
}
