/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Resolver.h"
#include "VerifyUtil.h"

namespace {
TEST_F(PreVerify, UsesNames) {
  // Base class A
  auto a = find_class_named(classes, "Lcom/facebook/redex/test/instr/ClassA;");
  ASSERT_NE(nullptr, a);

  auto method1 = find_vmethod_named(*a, "method1");
  ASSERT_NE(nullptr, method1);

  auto method0 = find_dmethod_named(*a, "method0");
  ASSERT_NE(nullptr, method0);

  auto field1 = find_field_named(*a, "aField1");
  ASSERT_NE(nullptr, field1);

  auto field2 = find_field_named(*a, "aField2");
  ASSERT_NE(nullptr, field2);

  // Sub class of A
  auto suba = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubA;");
  ASSERT_NE(nullptr, suba);

  auto method2 = find_vmethod_named(*suba, "method2");
  ASSERT_NE(nullptr, method2);

  auto field22 = find_field_named(*suba, "aField2");
  ASSERT_NE(nullptr, field22);

  // Field Class A is renamed
  auto a3 =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FieldAType;");
  ASSERT_NE(nullptr, a3);

  // Base Interface B
  auto b = find_class_named(classes, "Lcom/facebook/redex/test/instr/InterB;");
  ASSERT_NE(nullptr, b);

  auto method3 = find_vmethod_named(*b, "method3");
  ASSERT_NE(nullptr, method3);

  // Sub class of B
  auto subb = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubB;");
  ASSERT_NE(nullptr, subb);

  auto method4 = find_vmethod_named(*subb, "method3");
  ASSERT_NE(nullptr, method4);

  auto field4 = find_field_named(*subb, "bField4");
  ASSERT_NE(nullptr, field4);

  // Base Subclass C
  auto subc = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubC;");
  ASSERT_NE(nullptr, subc);

  auto method7 = find_vmethod_named(*subc, "method7");
  ASSERT_NE(nullptr, method7);

  auto field7 = find_field_named(*subc, "field7");
  ASSERT_NE(nullptr, field7);

  // Super class of C is renamed
  auto c = find_class_named(classes, "Lcom/facebook/redex/test/instr/C;");
  ASSERT_NE(nullptr, c);
}

TEST_F(PreVerify, UsesNamesTransitive) {
  // Base class D
  auto d = find_class_named(classes, "Lcom/facebook/redex/test/instr/ClassD;");
  ASSERT_NE(nullptr, d);

  auto method1 = find_vmethod_named(*d, "method9");
  ASSERT_NE(nullptr, method1);

  auto field1 = find_field_named(*d, "field1");
  ASSERT_NE(nullptr, field1);

  auto field2 = find_field_named(*d, "field2");
  ASSERT_NE(nullptr, field2);

  // Field Class is not renamed
  auto fd =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FieldDType;");
  ASSERT_NE(nullptr, d);

  auto method2 = find_vmethod_named(*fd, "method");
  ASSERT_NE(nullptr, method2);

  auto field3 = find_field_named(*fd, "field");
  ASSERT_NE(nullptr, field3);

  // D->FieldDType->D is woking properly
  auto field4 = find_field_named(*fd, "d2");
  ASSERT_NE(nullptr, field4);

  // Subclass of external-type field class is renamed
  auto fd2 =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FieldDType2;");
  ASSERT_NE(nullptr, fd2);
}

TEST_F(PreVerify, NoAnnotation) {
  auto renamed =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/NotUsed;");
  ASSERT_NE(nullptr, renamed);
}

TEST_F(PostVerify, UsesNames) {
  // Base class A
  auto a = find_class_named(classes, "Lcom/facebook/redex/test/instr/ClassA;");
  ASSERT_NE(nullptr, a);

  auto method1 = find_vmethod_named(*a, "method1");
  ASSERT_NE(nullptr, method1);

  auto method0 = find_dmethod_named(*a, "method0");
  ASSERT_NE(nullptr, method0);

  auto field1 = find_field_named(*a, "aField1");
  ASSERT_NE(nullptr, field1);

  auto field2 = find_field_named(*a, "aField2");
  ASSERT_NE(nullptr, field2);

  // Sub class of A
  auto suba = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubA;");
  ASSERT_NE(nullptr, suba);

  auto method2 = find_vmethod_named(*suba, "method2");
  ASSERT_NE(nullptr, method2);

  auto field22 = find_field_named(*suba, "aField2");
  ASSERT_NE(nullptr, field22);

  // Field Class A is renamed
  auto a3 =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FieldAType;");
  ASSERT_EQ(nullptr, a3);

  // Base Interface B
  auto b = find_class_named(classes, "Lcom/facebook/redex/test/instr/InterB;");
  ASSERT_NE(nullptr, b);

  auto method3 = find_vmethod_named(*b, "method3");
  ASSERT_NE(nullptr, method3);

  // Sub class of B
  auto subb = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubB;");
  ASSERT_NE(nullptr, subb);

  auto method4 = find_vmethod_named(*subb, "method3");
  ASSERT_NE(nullptr, method4);

  auto field4 = find_field_named(*subb, "bField4");
  ASSERT_NE(nullptr, field4);

  // Base Subclass C
  auto subc = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubC;");
  ASSERT_NE(nullptr, subc);

  auto method7 = find_vmethod_named(*subc, "method7");
  ASSERT_NE(nullptr, method7);

  auto field7 = find_field_named(*subc, "field7");
  ASSERT_NE(nullptr, field7);

  // Super class of C is renamed
  auto c = find_class_named(classes, "Lcom/facebook/redex/test/instr/C;");
  ASSERT_EQ(nullptr, c);
}

TEST_F(PostVerify, UsesNamesTransitive) {
  // Base class D
  auto d = find_class_named(classes, "Lcom/facebook/redex/test/instr/ClassD;");
  ASSERT_NE(nullptr, d);

  auto method1 = find_vmethod_named(*d, "method9");
  ASSERT_NE(nullptr, method1);

  auto field1 = find_field_named(*d, "field1");
  ASSERT_NE(nullptr, field1);

  auto field2 = find_field_named(*d, "field2");
  ASSERT_NE(nullptr, field2);

  // Field Class is not renamed
  auto fd =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FieldDType;");
  ASSERT_NE(nullptr, d);

  auto method2 = find_vmethod_named(*fd, "method");
  ASSERT_NE(nullptr, method2);

  auto field3 = find_field_named(*fd, "field");
  ASSERT_NE(nullptr, field3);

  // D->FieldDType->D is woking properly
  auto field4 = find_field_named(*fd, "d2");
  ASSERT_NE(nullptr, field4);

  // Subclass of external-type field class is renamed
  auto fd2 =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/FieldDType2;");
  ASSERT_EQ(nullptr, fd2);
}

TEST_F(PostVerify, NoAnnotation) {
  auto renamed =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/NotUsed;");
  ASSERT_EQ(nullptr, renamed);
}
} // namespace
