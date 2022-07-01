/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>

#include "verify/VerifyUtil.h"

using namespace testing;

TEST_F(PreVerify, VirtualMergingAB) {
  auto cls_a = find_class_named(classes, "Lcom/facebook/redextest/ClassA;");
  auto cls_b = find_class_named(classes, "Lcom/facebook/redextest/ClassB;");
  ASSERT_NE(cls_a, nullptr);
  ASSERT_NE(cls_b, nullptr);
  ASSERT_NE(find_vmethod_named(*cls_a, "do_something"), nullptr);
  ASSERT_NE(find_vmethod_named(*cls_b, "do_something"), nullptr);
}

TEST_F(PostVerify, VirtualMergingAB) {
  auto cls_a = find_class_named(classes, "Lcom/facebook/redextest/ClassA;");
  auto cls_b = find_class_named(classes, "Lcom/facebook/redextest/ClassB;");
  ASSERT_NE(cls_a, nullptr);
  ASSERT_NE(cls_b, nullptr);
  ASSERT_NE(find_vmethod_named(*cls_a, "do_something"), nullptr);
  ASSERT_EQ(find_vmethod_named(*cls_b, "do_something"), nullptr);
}

TEST_F(PreVerify, VirtualMergingCD) {
  auto cls_c = find_class_named(classes, "Lcom/facebook/redextest/ClassC;");
  auto cls_d = find_class_named(classes, "Lcom/facebook/redextest/ClassD;");
  ASSERT_NE(cls_c, nullptr);
  ASSERT_NE(cls_d, nullptr);
  ASSERT_NE(find_vmethod_named(*cls_c, "do_something"), nullptr);
  ASSERT_NE(find_vmethod_named(*cls_d, "do_something"), nullptr);
}

TEST_F(PostVerify, VirtualMergingCD) {
  auto cls_c = find_class_named(classes, "Lcom/facebook/redextest/ClassC;");
  auto cls_d = find_class_named(classes, "Lcom/facebook/redextest/ClassD;");
  ASSERT_NE(cls_c, nullptr);
  ASSERT_NE(cls_d, nullptr);
  ASSERT_NE(find_vmethod_named(*cls_c, "do_something"), nullptr);
  ASSERT_EQ(find_vmethod_named(*cls_d, "do_something"), nullptr);
}

TEST_F(PreVerify, VirtualMergingEF) {
  auto cls_e = find_class_named(classes, "Lcom/facebook/redextest/ClassE;");
  auto cls_f = find_class_named(classes, "Lcom/facebook/redextest/ClassF;");
  ASSERT_NE(cls_e, nullptr);
  ASSERT_NE(cls_f, nullptr);
  ASSERT_NE(find_vmethod_named(*cls_e, "do_something"), nullptr);
  ASSERT_NE(find_vmethod_named(*cls_f, "do_something"), nullptr);
}

TEST_F(PostVerify, VirtualMergingEF) {
  auto cls_e = find_class_named(classes, "Lcom/facebook/redextest/ClassE;");
  auto cls_f = find_class_named(classes, "Lcom/facebook/redextest/ClassF;");
  ASSERT_NE(cls_e, nullptr);
  ASSERT_NE(cls_f, nullptr);
  ASSERT_NE(find_vmethod_named(*cls_e, "do_something"), nullptr);
  ASSERT_EQ(find_vmethod_named(*cls_f, "do_something"), nullptr);
}

TEST_F(PreVerify, VirtualMergingGHIJ) {
  auto cls_g = find_class_named(classes, "Lcom/facebook/redextest/ClassG;");
  auto cls_h = find_class_named(classes, "Lcom/facebook/redextest/ClassH;");
  auto cls_i = find_class_named(classes, "Lcom/facebook/redextest/ClassI;");
  auto cls_j = find_class_named(classes, "Lcom/facebook/redextest/ClassJ;");
  ASSERT_NE(cls_g, nullptr);
  ASSERT_NE(cls_h, nullptr);
  ASSERT_NE(cls_i, nullptr);
  ASSERT_NE(cls_j, nullptr);
  ASSERT_NE(find_vmethod_named(*cls_g, "do_something"), nullptr);
  ASSERT_NE(find_vmethod_named(*cls_h, "do_something"), nullptr);
  ASSERT_NE(find_vmethod_named(*cls_i, "do_something"), nullptr);
  ASSERT_NE(find_vmethod_named(*cls_j, "do_something"), nullptr);
}

TEST_F(PostVerify, VirtualMergingGHIJ) {
  auto cls_g = find_class_named(classes, "Lcom/facebook/redextest/ClassG;");
  auto cls_h = find_class_named(classes, "Lcom/facebook/redextest/ClassH;");
  auto cls_i = find_class_named(classes, "Lcom/facebook/redextest/ClassI;");
  auto cls_j = find_class_named(classes, "Lcom/facebook/redextest/ClassJ;");
  ASSERT_NE(cls_g, nullptr);
  ASSERT_NE(cls_h, nullptr);
  ASSERT_NE(cls_i, nullptr);
  ASSERT_NE(cls_j, nullptr);
  ASSERT_NE(find_vmethod_named(*cls_g, "do_something"), nullptr);
  ASSERT_EQ(find_vmethod_named(*cls_h, "do_something"), nullptr);
  ASSERT_EQ(find_vmethod_named(*cls_i, "do_something"), nullptr);
  ASSERT_EQ(find_vmethod_named(*cls_j, "do_something"), nullptr);
}
