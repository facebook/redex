/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

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
  auto shape = find_class_named(
      classes, "Lcom/facebook/redex/AnonInterface1Shape1S0100000;");
  ASSERT_NE(shape, nullptr) << "Not find merged shape for Interface1\n";
  auto magic1 = find_vmethod_named(*shape, "magic1");
  ASSERT_NE(magic1, nullptr);
  auto magic2 = find_vmethod_named(*shape, "magic2");
  ASSERT_NE(magic2, nullptr);

  // This test covers external default method implementation.
  // https://developer.android.com/reference/java/lang/Iterable#spliterator()
  auto iterable_shape = find_class_named(
      classes, "Lcom/facebook/redex/AnonIterableShape0S0200000;");
  ASSERT_NE(iterable_shape, nullptr)
      << "Not find merged shape for java.lang.Iterable\n";
}
