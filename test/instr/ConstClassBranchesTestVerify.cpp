/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>

#include "verify/VerifyUtil.h"

using namespace testing;

namespace {
size_t count_switches(DexMethod* method) {
  size_t count = 0;
  method->balloon();
  auto code = method->get_code();
  for (auto& mie : InstructionIterable(code)) {
    if (opcode::is_switch(mie.insn->opcode())) {
      count++;
    }
  }
  return count;
}
} // namespace

TEST_F(PreVerify, VerifyBaseState) {
  auto cls_a =
      find_class_named(classes, "Lcom/facebook/redex/ConstClassBranches$A;");
  ASSERT_NE(cls_a, nullptr);
  auto method_a = find_dmethod_named(*cls_a, "get");
  ASSERT_NE(method_a, nullptr);
  EXPECT_EQ(count_switches(method_a), 0)
      << "Method does not match expected input state";

  auto cls_b =
      find_class_named(classes, "Lcom/facebook/redex/ConstClassBranches$B;");
  ASSERT_NE(cls_b, nullptr);
  auto method_b = find_dmethod_named(*cls_b, "get");
  ASSERT_NE(method_b, nullptr);
  EXPECT_EQ(count_switches(method_b), 0)
      << "Method does not match expected input state";

  auto cls_dup = find_class_named(
      classes, "Lcom/facebook/redex/ConstClassBranches$Duplicates;");
  ASSERT_NE(cls_dup, nullptr);
  auto method_dup = find_dmethod_named(*cls_dup, "get");
  ASSERT_NE(method_dup, nullptr);
  EXPECT_EQ(count_switches(method_dup), 0)
      << "Method does not match expected input state";

  auto cls_multi = find_class_named(
      classes, "Lcom/facebook/redex/ConstClassBranches$Complicated;");
  ASSERT_NE(cls_multi, nullptr);
  auto method_multi = find_dmethod_named(*cls_multi, "get");
  ASSERT_NE(method_multi, nullptr);
  EXPECT_EQ(count_switches(method_multi), 0)
      << "Method does not match expected input state";
}

TEST_F(PostVerify, VerifyTransformedA) {
  auto cls_a =
      find_class_named(classes, "Lcom/facebook/redex/ConstClassBranches$A;");
  ASSERT_NE(cls_a, nullptr);
  auto method_a = find_dmethod_named(*cls_a, "get");
  ASSERT_NE(method_a, nullptr);
  EXPECT_EQ(count_switches(method_a), 1) << "A.get should be transformed";
}

TEST_F(PostVerify, VerifyOriginalB) {
  auto cls_b =
      find_class_named(classes, "Lcom/facebook/redex/ConstClassBranches$B;");
  ASSERT_NE(cls_b, nullptr);
  auto method_b = find_dmethod_named(*cls_b, "get");
  ASSERT_NE(method_b, nullptr);
  EXPECT_EQ(count_switches(method_b), 0) << "B.get should not be transformed";
}

TEST_F(PostVerify, VerifyTransformedDuplicate) {
  auto cls_dup = find_class_named(
      classes, "Lcom/facebook/redex/ConstClassBranches$Duplicates;");
  ASSERT_NE(cls_dup, nullptr);
  auto method_dup = find_dmethod_named(*cls_dup, "get");
  ASSERT_NE(method_dup, nullptr);
  EXPECT_EQ(count_switches(method_dup), 1)
      << "Duplicates.get should be transformed";
}

TEST_F(PostVerify, VerifyTransformedMulti) {
  auto cls_multi = find_class_named(
      classes, "Lcom/facebook/redex/ConstClassBranches$Complicated;");
  ASSERT_NE(cls_multi, nullptr);
  auto method_multi = find_dmethod_named(*cls_multi, "get");
  ASSERT_NE(method_multi, nullptr);
  EXPECT_EQ(count_switches(method_multi), 2)
      << "Complicated.get should have two transforms applied";
}
