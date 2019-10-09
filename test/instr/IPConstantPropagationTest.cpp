/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "ControlFlow.h"
#include "DexInstruction.h"
#include "IRCode.h"
#include "VerifyUtil.h"

int count_igets(cfg::ControlFlowGraph& cfg, std::string field_name) {
  size_t num_igets = 0;
  for (const auto& mie : InstructionIterable(cfg)) {
    if (is_iget(mie.insn->opcode()) &&
        mie.insn->get_field()->get_name()->str() == field_name) {
      num_igets++;
    }
  }
  return num_igets;
}

TEST_F(PreVerify, IPConstantPropagation) {
  auto test_cls =
      find_class_named(classes, "Lredex/IPConstantPropagationTest;");
  size_t count = 0;
  for (auto& meth : test_cls->get_vmethods()) {
    IRCode* code = new IRCode(meth);
    ASSERT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    if (meth->get_name()->str() == "two_ctors") {
      EXPECT_EQ(2, count_igets(code->cfg(), "a"));
      EXPECT_EQ(2, count_igets(code->cfg(), "b"));
      ++count;
    } else if (meth->get_name()->str() == "modified_elsewhere") {
      EXPECT_EQ(1, count_igets(code->cfg(), "a"));
      EXPECT_EQ(1, count_igets(code->cfg(), "b"));
      ++count;
    }
  }
  // Make sure the two functions are there.
  ASSERT_EQ(count, 2);
}

TEST_F(PostVerify, IPConstantPropagation) {
  auto test_cls =
      find_class_named(classes, "Lredex/IPConstantPropagationTest;");
  size_t count = 0;
  for (auto& meth : test_cls->get_vmethods()) {
    IRCode* code = new IRCode(meth);
    ASSERT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    if (meth->get_name()->str() == "two_ctors") {
      EXPECT_EQ(0, count_igets(code->cfg(), "a"));
      EXPECT_EQ(2, count_igets(code->cfg(), "b"));
      ++count;
    } else if (meth->get_name()->str() == "modified_elsewhere") {
      EXPECT_EQ(0, count_igets(code->cfg(), "a"));
      EXPECT_EQ(1, count_igets(code->cfg(), "b"));
      ++count;
    }
  }
  // Make sure the two functions are there.
  ASSERT_EQ(count, 2);
}
