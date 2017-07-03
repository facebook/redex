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
#include "Transform.h"
#include "VerifyUtil.h"

TEST_F(PreVerify, ConstantPropagation) {
  TRACE(CONSTP, 1, "------------- pre ---------------\n");
  auto cls = find_class_named(classes, "Lredex/ConstantPropagationTest;");
  ASSERT_NE(cls, nullptr);
  for (auto& meth : cls->get_vmethods()) {
    if(meth->get_name()->str().find("if") == std::string::npos) {
      continue;
    }
    auto code = meth->get_dex_code();
    EXPECT_NE(code, nullptr);
    bool has_if = false;
    TRACE(CONSTP, 1, "%s\n", SHOW(meth));
    TRACE(CONSTP, 1, "%s\n", SHOW(code));
    for (const auto& insn : code->get_instructions()) {
      if (is_conditional_branch(insn->opcode())) {
        has_if = true;
      }
    }
    EXPECT_TRUE(has_if);
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
    auto code = meth->get_dex_code();
    EXPECT_NE(code, nullptr);
    bool has_if = false;
    TRACE(CONSTP, 1, "%s\n", SHOW(meth));
    TRACE(CONSTP, 1, "%s\n", SHOW(code));
    for (const auto& insn : code->get_instructions()) {
      if (is_conditional_branch(insn->opcode())) {
        has_if = true;
      }
    }
    EXPECT_FALSE(has_if);
  }
}
