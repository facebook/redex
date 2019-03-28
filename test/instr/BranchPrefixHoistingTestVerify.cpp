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

int count_ops(cfg::ControlFlowGraph& cfg, IROpcode op) {
  size_t result = 0;
  for (const auto& mie : InstructionIterable(cfg)) {
    if (mie.insn->opcode() == op) {
      result++;
    }
  }
  return result;
}

TEST_F(PreVerify, BranchPrefixHoisting) {
  TRACE(BPH, 1, "------------- pre ---------------\n");
  auto cls = find_class_named(classes, "Lredex/BranchPrefixHoistingTest;");
  ASSERT_NE(cls, nullptr);
  auto method = find_method_named(*cls, "testPrefixHoisting1");
  ASSERT_NE(method, nullptr);
  IRCode* code = new IRCode(method);
  code->build_cfg(true);
  EXPECT_EQ(11, count_ops(code->cfg(), OPCODE_INVOKE_VIRTUAL))
      << show(code->cfg());
}

TEST_F(PostVerify, BranchPrefixHoisting) {
  TRACE(BPH, 1, "------------- post ---------------\n");
  auto cls = find_class_named(classes, "Lredex/BranchPrefixHoistingTest;");
  ASSERT_NE(cls, nullptr);
  auto method = find_method_named(*cls, "testPrefixHoisting1");
  ASSERT_NE(method, nullptr);
  IRCode* code = new IRCode(method);
  code->build_cfg(true);
  EXPECT_EQ(9, count_ops(code->cfg(), OPCODE_INVOKE_VIRTUAL))
      << show(code->cfg());
}
