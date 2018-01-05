/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>
#include <string>

#include "DexInstruction.h"
#include "IRCode.h"
#include "VerifyUtil.h"

int count_ifs(IRCode* code) {
  size_t num_ifs = 0;
  for (const auto& mie : InstructionIterable(code)) {
    if (is_conditional_branch(mie.insn->opcode())) {
      num_ifs++;
    }
  }
  return num_ifs;
}

int count_ops(IRCode* code, IROpcode op) {
  size_t result = 0;
  for (const auto& mie : InstructionIterable(code)) {
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
    EXPECT_NE(code, nullptr);
    bool has_if = false;
    if (meth->get_name()->str().find("plus_one") != std::string::npos) {
      TRACE(CONSTP, 1, "%s\n", SHOW(meth));
      TRACE(CONSTP, 1, "%s\n", SHOW(code));
    }

    if (meth->get_name()->str().find("overflow") == std::string::npos) {
      EXPECT_EQ(1, count_ifs(code));
    }
    if (meth->get_name()->str().find("plus_one") != std::string::npos) {
      size_t num_add_lits = count_ops(code, OPCODE_ADD_INT_LIT8);
      EXPECT_EQ(1, count_ops(code, OPCODE_ADD_INT_LIT8));
    }
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
    if (meth->get_name()->str().find("plus_one") != std::string::npos) {
      TRACE(CONSTP, 1, "%s\n", SHOW(meth));
      TRACE(CONSTP, 1, "%s\n", SHOW(code));
    }

    EXPECT_EQ(0, count_ifs(code));
    if (meth->get_name()->str().find("plus_one") != std::string::npos) {
      EXPECT_EQ(0, count_ops(code, OPCODE_ADD_INT_LIT8));
    }

    if (meth->get_name()->str().find("overflow") != std::string::npos) {
      // make sure we don't fold overflow at compile time
      EXPECT_EQ(1, count_ops(code, OPCODE_ADD_INT_LIT8));
    }
  }
}
