/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/regex.hpp>

#include "Show.h"
#include "verify/VerifyUtil.h"

TEST_F(PreVerify, MergeablesExist) {
  auto* cls_1 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$1;");
  auto* cls_2 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$2;");
  auto* cls_3 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$3;");
  auto* cls_4 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$4;");
  auto* cls_5 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$5;");
  ASSERT_NE(cls_1, nullptr);
  ASSERT_NE(cls_2, nullptr);
  ASSERT_NE(cls_3, nullptr);
  ASSERT_NE(cls_4, nullptr);
  ASSERT_NE(cls_5, nullptr);

  // External default case
  auto* cls_6 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$6;");
  auto* cls_7 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$7;");
  ASSERT_NE(cls_6, nullptr);
  ASSERT_NE(cls_7, nullptr);
}

TEST_F(PostVerify, MergeablesRemoval) {
  auto* cls_1 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$1;");
  auto* cls_2 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$2;");
  auto* cls_3 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$3;");
  auto* cls_4 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$4;");
  auto* cls_5 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$5;");
  verify_class_merged(cls_1);
  verify_class_merged(cls_2);
  verify_class_merged(cls_3);
  verify_class_merged(cls_4);
  verify_class_merged(cls_5);

  // External default case
  auto* cls_6 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$6;");
  auto* cls_7 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$7;");
  verify_class_merged(cls_6);
  verify_class_merged(cls_7);
}

TEST_F(PostVerify, InterfaceMethodsOnShape) {
  boost::regex shape_name_pattern(
      "^Lcom/facebook/redex/AnonInterface1Shape_S0100000_\\w+;$");
  auto shape =
      find_class_named(classes, [&shape_name_pattern](const char* name) {
        return boost::regex_match(name, shape_name_pattern);
      });
  ASSERT_NE(shape, nullptr) << "Not find merged shape for Interface1\n";
  auto magic1 = find_vmethod_named(*shape, "magic1");
  ASSERT_NE(magic1, nullptr);
  auto magic2 = find_vmethod_named(*shape, "magic2");
  ASSERT_NE(magic2, nullptr);

  boost::regex comparator_shape_name_pattern(
      "^Lcom/facebook/redex/AnonComparatorShape_S0100000_\\w+;$");
  shape = find_class_named(
      classes, [&comparator_shape_name_pattern](const char* name) {
        return boost::regex_match(name, comparator_shape_name_pattern);
      });
  ASSERT_NE(shape, nullptr) << "Not find merged shape for Comparator\n";
  auto reversed = find_vmethod_named(*shape, "reversed");
  ASSERT_NE(reversed, nullptr);
}
