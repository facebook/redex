/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/regex.hpp>

#include "Show.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, MergeablesRemoval) {
  auto cls_1 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$1");
  auto cls_2 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$2");
  auto cls_3 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$3");
  auto cls_4 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$4");
  auto cls_5 = find_class_named(
      classes, "Lcom/facebook/redextest/AnonymousClassMergingTest$5");
  verify_type_erased(cls_1);
  verify_type_erased(cls_2);
  verify_type_erased(cls_3);
  verify_type_erased(cls_4);
  verify_type_erased(cls_5);
}

TEST_F(PostVerify, InterfaceMethodsOnShape) {
  boost::regex shape_name_pattern(
      "^Lcom/facebook/redex/AnonInterface1Shape[0-9]+S0100000;$");
  auto shape =
      find_class_named(classes, [&shape_name_pattern](const char* name) {
        return boost::regex_match(name, shape_name_pattern);
      });
  ASSERT_NE(shape, nullptr) << "Not find merged shape for Interface1\n";
  auto magic1 = find_vmethod_named(*shape, "magic1");
  ASSERT_NE(magic1, nullptr);
  auto magic2 = find_vmethod_named(*shape, "magic2");
  ASSERT_NE(magic2, nullptr);
}
