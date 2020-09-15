/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexInstruction.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "RedexTest.h"

struct IRCodeTest : public RedexTest {};

TEST_F(IRCodeTest, LoadParamInstructionsDirect) {
  using namespace dex_asm;

  auto method = DexMethod::make_method("Lfoo;", "bar", "V", {"I"})
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  auto code = std::make_unique<IRCode>(method, 3);
  auto it = code->begin();
  EXPECT_EQ(*it->insn, *dasm(IOPCODE_LOAD_PARAM, {3_v}));
  ++it;
  EXPECT_EQ(it, code->end());
}

TEST_F(IRCodeTest, LoadParamInstructionsVirtual) {
  using namespace dex_asm;

  auto method = DexMethod::make_method("Lfoo;", "bar", "V", {"I"})
                    ->make_concrete(ACC_PUBLIC, true);
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

TEST_F(IRCodeTest, try_region) {
  auto method = DexMethod::make_method("Lfoo;", "tryRegionTest", "V", {})
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto opcode = DOPCODE_CONST_WIDE_16;
  auto code = std::make_unique<IRCode>(method, 1);
  auto catz = new MethodItemEntry(DexType::make_type("Ljava/lang/Exception;"));
  auto op = new DexInstruction(opcode);
  code->push_back(*new MethodItemEntry(TRY_START, catz));
  uint32_t max = std::numeric_limits<uint16_t>::max();
  uint32_t num = max / op->size() + 17;
  EXPECT_FALSE(max % op->size() == 0);
  EXPECT_GT(num * op->size(), max);
  for (uint32_t i = 0; i < num; ++i) {
    code->push_back(*new MethodItemEntry(new DexInstruction(opcode)));
  }
  code->push_back(*new MethodItemEntry(TRY_END, catz));
  code->push_back(*catz);

  method->set_code(std::move(code));
  auto dex_code = method->get_code()->sync(method);

  const auto& tries = dex_code->get_tries();
  EXPECT_EQ(2, tries.size());
  const auto& first = tries[0];
  EXPECT_EQ(0, first->m_start_addr);
  const auto split = max - (max % op->size());
  EXPECT_NE(split, max);
  EXPECT_EQ(split, first->m_insn_count);

  const auto& second = tries[1];
  EXPECT_EQ(split, second->m_start_addr);
  EXPECT_EQ(num * op->size() - split, second->m_insn_count);
}
