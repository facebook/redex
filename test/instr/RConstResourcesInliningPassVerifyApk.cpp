/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "verify/VerifyUtil.h"

TEST_F(PostVerify, ResourcesInliningPassTest_DexPatching) {
  auto* cls = find_class_named(classes, "Lcom/fb/resources/MainActivity;");
  // COPY PASTA WITH LINE NUMS/OPCODES ADJUSTED. MAKE THIS NICER.
  auto* method = find_method_named(*cls, "logValues");
  IRCode* code = new IRCode(method);
  code->build_cfg();
  auto& cfg = code->cfg();
  for (auto* block : cfg.blocks()) {
    int line_num = 0;
    for (auto& mie : InstructionIterable(block)) {
      line_num++;
      auto* insn = mie.insn;
      if (block->id() == 0) {
        if (line_num == 5) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(insn->get_literal(), 1);
        }
      } else if (block->id() == 1) {
        if (line_num == 2) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(uint32_t(insn->get_literal()), 0xFFFF0000);
        } else if (line_num == 18) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(uint32_t(insn->get_literal()), 0xFF673AB7);
        } else if (line_num == 48) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(uint32_t(insn->get_literal()), 3);
        } else if (line_num == 50) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string = insn->get_string()->str();
          ASSERT_EQ(string.substr(0, 6), "Hello,");
        } else if (line_num == 51) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        }
      } else if (block->id() == 4) {
        if (line_num == 28) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST);
          ASSERT_EQ(uint32_t(insn->get_literal()), 0xFFFFFFFF);
        } else if (line_num == 42) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string = insn->get_string()->str();
          ASSERT_EQ(string, "#ff673ab7");
        } else if (line_num == 43) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        } else if (line_num == 55) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string = insn->get_string()->str();
          ASSERT_EQ(string, "3");
        } else if (line_num == 56) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        } else if (line_num == 68) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string = insn->get_string()->str();
          ASSERT_EQ(string, "com.fb.resources:integer/loop_count");
        } else if (line_num == 69) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        } else if (line_num == 81) {
          ASSERT_EQ(insn->opcode(), OPCODE_CONST_STRING);
          auto string = insn->get_string()->str();
          ASSERT_EQ(string, "loop_count");
        } else if (line_num == 82) {
          ASSERT_EQ(insn->opcode(), IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
        }
      }
    }
  }
}
