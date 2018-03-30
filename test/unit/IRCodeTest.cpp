/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "InstructionLowering.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"

struct IRCodeTest : public RedexTest {};

std::ostream& operator<<(std::ostream& os, const IRInstruction& to_show) {
  return os << show(&to_show);
}

TEST_F(IRCodeTest, LoadParamInstructionsDirect) {
  using namespace dex_asm;

  auto method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "V", {"I"}));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto code = std::make_unique<IRCode>(method, 3);
  auto it = code->begin();
  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM, {3_v}));
  ++it;
  EXPECT_EQ(it, code->end());
}

TEST_F(IRCodeTest, LoadParamInstructionsVirtual) {
  using namespace dex_asm;

  auto method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "V", {"I"}));
  method->make_concrete(ACC_PUBLIC, true);
  auto code = std::make_unique<IRCode>(method, 3);
  auto it = code->begin();
  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM_OBJECT, {3_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM, {4_v}));
  ++it;
  EXPECT_EQ(it, code->end());
}

TEST_F(IRCodeTest, infinite_loop) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LBaz;.bar:()V"
      (
        (:lbl)
        (goto :lbl)
      )
    )
  )");

  auto code = method->get_code();
  std::cout << show(code) << std::endl;

  instruction_lowering::lower(method);
  auto dex_code = code->sync(method);

  const auto& insns = dex_code->get_instructions();
  EXPECT_EQ(2, insns.size());
  auto it = insns.begin();
  EXPECT_EQ(DOPCODE_NOP, (*it)->opcode());
  ++it;
  EXPECT_EQ(DOPCODE_GOTO, (*it)->opcode());
}

TEST_F(IRCodeTest, useless_goto) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LBaz;.bar:()V"
      (
        (const v0 0)
        (goto :lbl)
        (:lbl)
        (const v1 1)
      )
    )
  )");

  auto code = method->get_code();
  std::cout << show(code) << std::endl;

  instruction_lowering::lower(method);
  auto dex_code = code->sync(method);

  const auto& insns = dex_code->get_instructions();
  EXPECT_EQ(2, insns.size());
  auto it = insns.begin();
  EXPECT_EQ(DOPCODE_CONST_4, (*it)->opcode());
  ++it;
  EXPECT_EQ(DOPCODE_CONST_4, (*it)->opcode());
}

TEST_F(IRCodeTest, useless_if) {
  auto method = assembler::method_from_string(R"(
    (method (public) "LBaz;.bar:()V"
      (
        (const v0 0)
        (if-gtz v0 :lbl)
        (:lbl)
        (const v1 1)
      )
    )
  )");

  auto code = method->get_code();
  std::cout << show(code) << std::endl;

  instruction_lowering::lower(method);
  auto dex_code = code->sync(method);

  const auto& insns = dex_code->get_instructions();
  EXPECT_EQ(2, insns.size());
  auto it = insns.begin();
  EXPECT_EQ(DOPCODE_CONST_4, (*it)->opcode());
  ++it;
  EXPECT_EQ(DOPCODE_CONST_4, (*it)->opcode());
}
