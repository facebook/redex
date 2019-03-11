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

int count_ifs(cfg::ControlFlowGraph& cfg) {
  size_t num_ifs = 0;
  for (const auto& mie : InstructionIterable(cfg)) {
    if (is_conditional_branch(mie.insn->opcode())) {
      num_ifs++;
    }
  }
  return num_ifs;
}

int count_ops(cfg::ControlFlowGraph& cfg, IROpcode op) {
  size_t result = 0;
  for (const auto& mie : InstructionIterable(cfg)) {
    if (mie.insn->opcode() == op) {
      result++;
    }
  }
  return result;
}

TEST_F(PreVerify, ConstantPropagation) {
  TRACE(CONSTP, 1, "------------- pre ---------------\n");
  auto cls = find_class_named(classes, "Lredex/ConstantPropagationTest;");
  ASSERT_NE(cls, nullptr);
  for (auto& meth : cls->get_vmethods()) {
    if (meth->get_name()->str().find("if") == std::string::npos) {
      continue;
    }
    IRCode* code = new IRCode(meth);
    ASSERT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    bool has_if = false;
    if (meth->get_name()->str().find("plus_one") != std::string::npos) {
      TRACE(CONSTP, 1, "%s\n", SHOW(meth));
      TRACE(CONSTP, 1, "%s\n", SHOW(code));
    }

    if (meth->get_name()->str().find("overflow") == std::string::npos) {
      EXPECT_EQ(1, count_ifs(code->cfg()));
    }
    if (meth->get_name()->str().find("plus_one") != std::string::npos) {
      EXPECT_EQ(1, count_ops(code->cfg(), OPCODE_ADD_INT_LIT8));
    }
    if (meth->get_name()->str().find("lit_minus") != std::string::npos) {
      EXPECT_EQ(1, count_ops(code->cfg(), OPCODE_RSUB_INT_LIT8));
    }
    if (meth->get_name()->str().find("multiply_lit_const") !=
        std::string::npos) {
      EXPECT_EQ(1, count_ops(code->cfg(), OPCODE_MUL_INT_LIT8));
    }
    if (meth->get_name()->str().find("multiply_large_lit_const") !=
        std::string::npos) {
      EXPECT_EQ(1, count_ops(code->cfg(), OPCODE_MUL_INT_LIT16));
    }
    if (meth->get_name()->str().find("shl_lit_const") != std::string::npos) {
      EXPECT_EQ(1, count_ops(code->cfg(), OPCODE_SHL_INT_LIT8));
    }
    if (meth->get_name()->str().find("if_shr_lit_const") != std::string::npos) {
      EXPECT_EQ(1, count_ops(code->cfg(), OPCODE_SHR_INT_LIT8));
    }
    if (meth->get_name()->str().find("ushr_lit_const") != std::string::npos) {
      EXPECT_EQ(1, count_ops(code->cfg(), OPCODE_USHR_INT_LIT8));
    }
    if (meth->get_name()->str().find("modulo_3") != std::string::npos) {
      EXPECT_EQ(1, count_ops(code->cfg(), OPCODE_REM_INT_LIT8));
    }
    code->clear_cfg();
  }
}

TEST_F(PostVerify, ConstantPropagation) {
  TRACE(CONSTP, 1, "------------- post ---------------\n");
  auto cls = find_class_named(classes, "Lredex/ConstantPropagationTest;");
  ASSERT_NE(cls, nullptr);
  for (auto& meth : cls->get_vmethods()) {
    if(meth->get_name()->str().find("if") == std::string::npos) {
      continue;
    }
    IRCode* code = new IRCode(meth);
    EXPECT_NE(code, nullptr);
    code->build_cfg(/* editable */ true);
    if (meth->get_name()->str().find("plus_one") != std::string::npos) {
      TRACE(CONSTP, 1, "%s\n", SHOW(meth));
      TRACE(CONSTP, 1, "%s\n", SHOW(code));
    }

    EXPECT_EQ(0, count_ifs(code->cfg()));
    if (meth->get_name()->str().find("overflow") != std::string::npos) {
      // make sure we don't fold overflow at compile time
      EXPECT_EQ(1, count_ops(code->cfg(), OPCODE_ADD_INT_LIT8));
    }
    if (meth->get_name()->str().find("plus_one") != std::string::npos) {
      EXPECT_EQ(0, count_ops(code->cfg(), OPCODE_ADD_INT_LIT8));
    }
    if (meth->get_name()->str().find("lit_minus") != std::string::npos) {
      EXPECT_EQ(0, count_ops(code->cfg(), OPCODE_RSUB_INT_LIT8));
    }
    if (meth->get_name()->str().find("multiply_lit_const") !=
        std::string::npos) {
      EXPECT_EQ(0, count_ops(code->cfg(), OPCODE_MUL_INT_LIT8));
    }
    if (meth->get_name()->str().find("multiply_large_lit_const") !=
        std::string::npos) {
      EXPECT_EQ(0, count_ops(code->cfg(), OPCODE_MUL_INT_LIT16));
    }
    if (meth->get_name()->str().find("shl_lit_const") != std::string::npos) {
      EXPECT_EQ(0, count_ops(code->cfg(), OPCODE_SHL_INT_LIT8));
    }
    if (meth->get_name()->str().find("if_shr_lit_const") != std::string::npos) {
      EXPECT_EQ(0, count_ops(code->cfg(), OPCODE_SHR_INT_LIT8));
    }
    if (meth->get_name()->str().find("ushr_lit_const") != std::string::npos) {
      EXPECT_EQ(0, count_ops(code->cfg(), OPCODE_USHR_INT_LIT8));
    }
    if (meth->get_name()->str().find("modulo_3") != std::string::npos) {
      EXPECT_EQ(0, count_ops(code->cfg(), OPCODE_REM_INT_LIT8));
    }
    code->clear_cfg();
  }
}
