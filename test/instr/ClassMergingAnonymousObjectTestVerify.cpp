/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "verify/VerifyUtil.h"

TEST_F(PostVerify, MergeablesRemoval) {
  auto* sa1 = find_class_named(
      classes,
      "Lcom/facebook/redextest/ClassMergingAnonymousObjectTest$SubA1;");
  auto* sa2 = find_class_named(
      classes,
      "Lcom/facebook/redextest/ClassMergingAnonymousObjectTest$SubA2;");
  auto* sa3 = find_class_named(
      classes,
      "Lcom/facebook/redextest/ClassMergingAnonymousObjectTest$SubA3;");
  auto* anon1 = find_class_named(
      classes,
      "Lcom/facebook/redextest/ClassMergingAnonymousObjectTest$getSubA1$1;");
  auto* anon2 = find_class_named(
      classes,
      "Lcom/facebook/redextest/ClassMergingAnonymousObjectTest$getSubA12$1;");
  // SubA1 is not merged since it has anonymous object children
  ASSERT_NE(sa1, nullptr);
  verify_class_merged(sa2);
  verify_class_merged(sa3);
  // Anonymous objects are not merged as expected
  ASSERT_NE(anon1, nullptr);
  ASSERT_NE(anon2, nullptr);
}
