/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Show.h"
#include "VerifyUtil.h"

namespace {
size_t count_empty_const_str(DexMethod* method) {
  size_t count = 0;
  method->balloon();
  auto code = method->get_code();
  for (auto& mie : InstructionIterable(code)) {
    if (mie.insn->opcode() == OPCODE_CONST_STRING &&
        mie.insn->get_string()->str().empty()) {
      count++;
    }
  }
  return count;
}
} // namespace

TEST_F(PreVerify, ValueOfOptHasEmptyStr) {
  auto anno_cls =
      find_class_named(classes, "Lcom/facebook/redex/TestStringDef;");
  ASSERT_NE(anno_cls, nullptr);
  auto util_cls =
      find_class_named(classes, "Lcom/facebook/redex/TestStringDef$Util;");
  ASSERT_NE(util_cls, nullptr);

  auto value_of = find_dmethod_named(*util_cls, "valueOf");
  ASSERT_NE(value_of, nullptr);

  auto value_of_opt = find_dmethod_named(*util_cls, "valueOfOpt");
  ASSERT_NE(value_of_opt, nullptr);
  EXPECT_EQ(count_empty_const_str(value_of_opt), 1);
}

TEST_F(PostVerify, ValueOfOptHasNoEmptyStr) {
  auto anno_cls =
      find_class_named(classes, "Lcom/facebook/redex/TestStringDef;");
  ASSERT_NE(anno_cls, nullptr);
  auto util_cls =
      find_class_named(classes, "Lcom/facebook/redex/TestStringDef$Util;");
  ASSERT_NE(util_cls, nullptr);

  auto value_of = find_dmethod_named(*util_cls, "valueOf");
  ASSERT_NE(value_of, nullptr);

  auto value_of_opt = find_dmethod_named(*util_cls, "valueOfOpt");
  ASSERT_NE(value_of_opt, nullptr);
  EXPECT_EQ(count_empty_const_str(value_of_opt), 0);
}

TEST_F(PreVerify, TestValueOfString) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redex/TypedefAnnoOptTest;");
  ASSERT_NE(cls, nullptr);

  auto m = find_dmethod_named(*cls, "testValueOfString");
  ASSERT_NE(m, nullptr);

  m->balloon();
  for (const auto& mie : InstructionIterable(m->get_code())) {
    auto* insn = mie.insn;
    if (insn->opcode() == OPCODE_INVOKE_STATIC) {
      auto invoked_method = insn->get_method();
      EXPECT_EQ(invoked_method->get_name()->str(), "valueOf");
    }
  }
}

TEST_F(PostVerify, TestValueOfStringOpt) {
  auto cls =
      find_class_named(classes, "Lcom/facebook/redex/TypedefAnnoOptTest;");
  ASSERT_NE(cls, nullptr);

  auto m = find_dmethod_named(*cls, "testValueOfString");
  ASSERT_NE(m, nullptr);

  m->balloon();
  for (const auto& mie : InstructionIterable(m->get_code())) {
    auto* insn = mie.insn;
    if (insn->opcode() == OPCODE_INVOKE_STATIC) {
      auto invoked_method = insn->get_method();
      EXPECT_EQ(invoked_method->get_name()->str(), "valueOfOpt");
    }
  }
}
