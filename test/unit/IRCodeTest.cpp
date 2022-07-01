/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <iostream>

#include "DexAsm.h"
#include "DexInstruction.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "RedexTest.h"
#include "Show.h"

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

namespace {

DexMethod* construct_switch_payload(const char* method_name,
                                    size_t target_count,
                                    int32_t target_multiplier) {
  auto method = DexMethod::make_method("Lfoo;", method_name, "V", {"I"})
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = std::make_unique<IRCode>(method, 1);
  code->build_cfg();
  auto& cfg = code->cfg();

  auto entry = cfg.entry_block();

  auto ret_block = cfg.create_block();
  ret_block->push_back(new IRInstruction(IROpcode::OPCODE_RETURN_VOID));

  std::vector<std::pair<int32_t, cfg::Block*>> targets;

  targets.reserve(target_count);
  for (size_t i = 0; i < target_count; ++i) {
    auto target_block = cfg.create_block();
    // Need an instruction so it does not get removed.
    auto dummy_insn = new IRInstruction(IROpcode::OPCODE_CONST);
    dummy_insn->set_dest(0);
    dummy_insn->set_literal(i); // Not really necessary, could use `0`.
    target_block->push_back(dummy_insn);

    cfg.add_edge(target_block, ret_block, cfg::EdgeType::EDGE_GOTO);
    targets.emplace_back(i * target_multiplier, target_block);
  }

  IRInstruction* switch_insn = new IRInstruction(IROpcode::OPCODE_SWITCH);
  switch_insn->set_src(0, 0);
  cfg.create_branch(entry, switch_insn, ret_block, targets);
  cfg.recompute_registers_size();

  code->clear_cfg();

  method->set_code(std::move(code));

  instruction_lowering::lower(method);
  method->sync();

  return method;
}

} // namespace

TEST_F(IRCodeTest, encode_large_sparse_switch) {
  constexpr size_t kTargetCount = 40000;
  constexpr int32_t kTargetMultiplier = 10; // Large enough gaps...

  auto method = construct_switch_payload(
      "largeSparseSwitch", kTargetCount, kTargetMultiplier);

  auto dex_code = method->get_dex_code();
  ASSERT_NE(dex_code, nullptr);

  auto& dex_insns = dex_code->get_instructions();
  ASSERT_GT(dex_insns.size(), 0u);

  auto it = std::find_if(dex_insns.begin(), dex_insns.end(), [](auto i) {
    return i->opcode() == FOPCODE_SPARSE_SWITCH;
  });
  ASSERT_TRUE(it != dex_insns.end());

  DexOpcodeData* dod = static_cast<DexOpcodeData*>(*it);

  EXPECT_EQ(dod->data_size(), 1 + 4 * kTargetCount);
  EXPECT_EQ(dod->data()[0], kTargetCount);
}

TEST_F(IRCodeTest, encode_large_packed_switch) {
  constexpr size_t kTargetCount = 40000;
  constexpr int32_t kTargetMultiplier = 1; // No gaps...

  auto method = construct_switch_payload(
      "largePackedSwitch", kTargetCount, kTargetMultiplier);

  auto dex_code = method->get_dex_code();
  ASSERT_NE(dex_code, nullptr);

  auto& dex_insns = dex_code->get_instructions();
  ASSERT_GT(dex_insns.size(), 0u);

  auto it = std::find_if(dex_insns.begin(), dex_insns.end(), [](auto i) {
    return i->opcode() == FOPCODE_PACKED_SWITCH;
  });
  ASSERT_TRUE(it != dex_insns.end());

  DexOpcodeData* dod = static_cast<DexOpcodeData*>(*it);

  EXPECT_EQ(dod->data_size(), 1 + 2 + 2 * kTargetCount);
  EXPECT_EQ(dod->data()[0], kTargetCount);
}
