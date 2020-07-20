/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Resolver.h"
#include "VerifyUtil.h"

namespace {
TEST_F(PreVerify, ClassAndMember) {
  auto a = find_class_named(classes, "Lcom/facebook/redex/test/instr/ClassA;");
  ASSERT_NE(nullptr, a);

  auto method1 = find_vmethod_named(*a, "method1");
  ASSERT_NE(nullptr, method1);

  auto method0 = find_dmethod_named(*a, "method0");
  ASSERT_NE(nullptr, method0);

  auto field1 = find_field_named(*a, "aField1");
  ASSERT_NE(nullptr, field1);

  auto b = find_class_named(classes, "Lcom/facebook/redex/test/instr/InterB;");
  ASSERT_NE(nullptr, b);

  auto method3 = find_vmethod_named(*b, "method3");
  ASSERT_NE(nullptr, method3);

  auto c = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubC;");
  ASSERT_NE(nullptr, c);

  auto method7 = find_vmethod_named(*c, "method7");
  ASSERT_NE(nullptr, method7);

  auto field7 = find_field_named(*c, "field7");
  ASSERT_NE(nullptr, field7);
}

TEST_F(PreVerify, SubclassAndMember) {
  auto suba = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubA;");
  ASSERT_NE(nullptr, suba);

  auto method2 = find_vmethod_named(*suba, "method2");
  ASSERT_NE(nullptr, method2);

  auto field2 = find_field_named(*suba, "aField2");
  ASSERT_NE(nullptr, field2);

  auto subb = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubB;");
  ASSERT_NE(nullptr, subb);

  auto method3 = find_vmethod_named(*subb, "method3");
  ASSERT_NE(nullptr, method3);

  auto field4 = find_field_named(*subb, "bField4");
  ASSERT_NE(nullptr, field4);
}

TEST_F(PreVerify, NoAnnotation) {
  auto renamed =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Renamed;");
  ASSERT_NE(nullptr, renamed);
}

TEST_F(PreVerify, Superclass) {
  auto c = find_class_named(classes, "Lcom/facebook/redex/test/instr/C;");
  ASSERT_NE(nullptr, c);
}

TEST_F(PostVerify, ClassAndMember) {
  auto a = find_class_named(classes, "Lcom/facebook/redex/test/instr/ClassA;");
  ASSERT_NE(nullptr, a);

  auto method1 = find_vmethod_named(*a, "method1");
  ASSERT_NE(nullptr, method1);

  auto method0 = find_dmethod_named(*a, "method0");
  ASSERT_NE(nullptr, method0);

  auto field1 = find_field_named(*a, "aField1");
  ASSERT_NE(nullptr, field1);

  auto b = find_class_named(classes, "Lcom/facebook/redex/test/instr/InterB;");
  ASSERT_NE(nullptr, b);

  auto method3 = find_vmethod_named(*b, "method3");
  ASSERT_NE(nullptr, method3);

  auto c = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubC;");
  ASSERT_NE(nullptr, c);

  auto method7 = find_vmethod_named(*c, "method7");
  ASSERT_NE(nullptr, method7);

  auto field7 = find_field_named(*c, "field7");
  ASSERT_NE(nullptr, field7);
}

TEST_F(PostVerify, SubclassAndMember) {
  auto suba = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubA;");
  ASSERT_NE(nullptr, suba);

  auto method2 = find_vmethod_named(*suba, "method2");
  ASSERT_NE(nullptr, method2);

  auto field2 = find_field_named(*suba, "aField2");
  ASSERT_NE(nullptr, field2);

  auto subb = find_class_named(classes, "Lcom/facebook/redex/test/instr/SubB;");
  ASSERT_NE(nullptr, subb);

  auto method3 = find_vmethod_named(*subb, "method3");
  ASSERT_NE(nullptr, method3);

  auto field4 = find_field_named(*subb, "bField4");
  ASSERT_NE(nullptr, field4);
}

TEST_F(PostVerify, NoAnnotation) {
  auto renamed =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Renamed;");
  ASSERT_EQ(nullptr, renamed);
}

TEST_F(PostVerify, Superclass) {
  auto c = find_class_named(classes, "Lcom/facebook/redex/test/instr/C;");
  ASSERT_EQ(nullptr, c);
}
} // namespace
