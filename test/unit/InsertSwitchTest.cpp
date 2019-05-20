/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <iterator>

#include "ControlFlow.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IntroduceSwitch.h"

struct InsertSwitchTest : testing::Test {
  DexMethod* m_method;

  InsertSwitchTest() {
    g_redex = new RedexContext();
    auto args = DexTypeList::make_type_list({});
    auto proto = DexProto::make_proto(get_void_type(), args);
    m_method = static_cast<DexMethod*>(DexMethod::make_method(
        get_object_type(), DexString::make_string("testMethod"), proto));
    m_method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    m_method->set_code(std::make_unique<IRCode>(m_method, 1));
  }

  std::pair<MethodItemEntry*, BranchTarget*> create_goto() {
    using namespace dex_asm;

    auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO));
    auto target = new BranchTarget(goto_mie);
    return {goto_mie, target};
  }

  std::pair<std::pair<IRInstruction*, MethodItemEntry*>, BranchTarget*>
  create_if_ne(dex_asm::Operand switch_arg, dex_asm::Operand constant) {
    using namespace dex_asm;
    auto const_set = dasm(OPCODE_CONST, {0_v, constant});
    auto if_ne = new MethodItemEntry(dasm(OPCODE_IF_NE, {0_v, switch_arg}));
    auto target = new BranchTarget(if_ne);
    return {{const_set, if_ne}, target};
  }

  void clear_method_code() {
    m_method->set_code(std::make_unique<IRCode>(m_method, 1));
  }

  ~InsertSwitchTest() { delete g_redex; }
};

// Code:    if r == i then A else if r == i+1 then B else if r == i+2 then C; D
// Result:  switch r {ABC}; D
TEST_F(InsertSwitchTest, simpleCompactSwitch) {
  using namespace dex_asm;
  clear_method_code();

  auto gt1End = create_goto();
  auto gt2End = create_goto();
  auto gt3End = create_goto();
  auto gt4End = create_goto();

  auto code = m_method->get_code();

  code->push_back(dasm(OPCODE_CONST, {4_v, 2_L}));

  auto first_branch = create_if_ne(3_v, 0_L);
  auto second_branch = create_if_ne(3_v, 1_L);
  auto third_branch = create_if_ne(3_v, 2_L);

  // let's have an infinite loop so that the last block has a succ
  code->push_back(gt4End.second);

  code->push_back(first_branch.first.first /* set constant*/);
  code->push_back(*first_branch.first.second /* branch */);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 1_v, 1_v}));
  code->push_back(*gt1End.first);

  code->push_back(first_branch.second /* branch target*/);
  code->push_back(second_branch.first.first);
  code->push_back(*second_branch.first.second);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 2_v, 2_v}));
  code->push_back(*gt2End.first);

  code->push_back(second_branch.second);
  code->push_back(third_branch.first.first);
  code->push_back(*third_branch.first.second);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 4_v, 4_v}));
  code->push_back(*gt3End.first);

  code->push_back(third_branch.second);
  code->push_back(dasm(OPCODE_NOP));

  code->push_back(gt1End.second);
  code->push_back(gt2End.second);
  code->push_back(gt3End.second);
  code->push_back(dasm(OPCODE_ADD_INT, {4_v, 4_v, 4_v}));
  code->push_back(*gt4End.first);

  code->set_registers_size(5);

  printf("Original code: %s\n", SHOW(m_method->get_code()));
  IntroduceSwitchPass().run(m_method);
  printf("Result code: %s\n", SHOW(m_method->get_code()));
  m_method->get_code()->build_cfg(/* editable */ false);

  printf("Result cfg: %s\n", SHOW(m_method->get_code()->cfg()));

  auto final_code = m_method->get_code();
  auto switch_insn_OPCODE = OPCODE_NOP;
  for (auto i = final_code->begin(); i != final_code->end(); i++) {
    if (i->insn->opcode() == OPCODE_PACKED_SWITCH) {
      switch_insn_OPCODE = OPCODE_PACKED_SWITCH;
    } else if (i->insn->opcode() == OPCODE_SPARSE_SWITCH) {
      switch_insn_OPCODE = OPCODE_SPARSE_SWITCH;
    }
  }
  EXPECT_EQ(OPCODE_PACKED_SWITCH, switch_insn_OPCODE);
  EXPECT_EQ(6, final_code->cfg().blocks().size());
  EXPECT_EQ(10, final_code->count_opcodes());
}

// Code:    if r==i A else if r==i+10 B else if r==i+2 C
// Result:  switch r {ABC}
TEST_F(InsertSwitchTest, simplifySparseSwitch) {
  using namespace dex_asm;

  clear_method_code();

  auto gt1End = create_goto();
  auto gt2End = create_goto();
  auto gt3End = create_goto();
  auto gt4End = create_goto();

  auto code = m_method->get_code();

  code->push_back(dasm(OPCODE_CONST, {4_v, 2_L}));

  auto first_branch = create_if_ne(3_v, 0_L);
  auto second_branch = create_if_ne(3_v, 10_L);
  auto third_branch = create_if_ne(3_v, 2_L);

  // let's have an infinite loop so that the last block has a succ
  code->push_back(gt4End.second);

  code->push_back(first_branch.first.first /* set constant*/);
  code->push_back(*first_branch.first.second /* branch */);
  code->push_back(dasm(OPCODE_ADD_INT, {1_v, 1_v, 1_v}));
  code->push_back(*gt1End.first);

  code->push_back(first_branch.second /* branch target*/);
  code->push_back(second_branch.first.first);
  code->push_back(*second_branch.first.second);
  code->push_back(dasm(OPCODE_ADD_INT, {2_v, 2_v, 2_v}));
  code->push_back(*gt2End.first);

  code->push_back(second_branch.second);
  code->push_back(third_branch.first.first);
  code->push_back(*third_branch.first.second);
  code->push_back(dasm(OPCODE_ADD_INT, {1_v, 2_v, 1_v}));
  code->push_back(*gt3End.first);

  code->push_back(third_branch.second);
  code->push_back(dasm(OPCODE_NOP));

  code->push_back(gt1End.second);
  code->push_back(gt2End.second);
  code->push_back(gt3End.second);
  code->push_back(dasm(OPCODE_ADD_INT, {4_v, 1_v, 2_v}));
  code->push_back(*gt4End.first);

  code->set_registers_size(5);

  printf("Original code: %s\n", SHOW(m_method->get_code()));
  m_method->get_code()->build_cfg(/* editable */ false);
  printf("Result cfg: %s\n", SHOW(m_method->get_code()->cfg()));

  IntroduceSwitchPass().run(m_method);
  printf("Result code: %s\n", SHOW(m_method->get_code()));
  m_method->get_code()->build_cfg(/* editable */ false);

  printf("Result cfg: %s\n", SHOW(m_method->get_code()->cfg()));

  auto final_code = m_method->get_code();
  auto switch_insn_OPCODE = OPCODE_NOP;
  for (auto i = final_code->begin(); i != final_code->end(); i++) {
    if (i->insn->opcode() == OPCODE_PACKED_SWITCH) {
      switch_insn_OPCODE = OPCODE_PACKED_SWITCH;
    } else if (i->insn->opcode() == OPCODE_SPARSE_SWITCH) {
      switch_insn_OPCODE = OPCODE_SPARSE_SWITCH;
    }
  }
  EXPECT_EQ(OPCODE_SPARSE_SWITCH, switch_insn_OPCODE);
  EXPECT_EQ(6, final_code->cfg().blocks().size());
  EXPECT_EQ(10, final_code->count_opcodes());
}

// Code:    if r==i A else if r==i+10 B
// Result:  no change
TEST_F(InsertSwitchTest, skipSmallChain) {
  using namespace dex_asm;
  clear_method_code();

  auto gt1End = create_goto();
  auto gt2End = create_goto();
  auto gt3End = create_goto();

  auto code = m_method->get_code();

  code->push_back(dasm(OPCODE_CONST, {3_v, 2_L}));

  auto first_branch = create_if_ne(3_v, 0_L);
  auto second_branch = create_if_ne(3_v, 10_L);

  code->push_back(gt3End.second);
  code->push_back(first_branch.first.first /* set constant*/);
  code->push_back(*first_branch.first.second /* branch */);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 0_v, 0_v}));
  code->push_back(*gt1End.first);

  code->push_back(first_branch.second /* branch target*/);
  code->push_back(second_branch.first.first);
  code->push_back(*second_branch.first.second);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 0_v, 1_v}));
  code->push_back(*gt2End.first);

  code->push_back(second_branch.second);
  code->push_back(dasm(OPCODE_NOP));

  code->push_back(gt1End.second);
  code->push_back(gt2End.second);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 0_v, 0_v}));
  code->push_back(*gt3End.first);
  code->set_registers_size(4);

  printf("Original code: %s\n", SHOW(m_method->get_code()));
  IntroduceSwitchPass().run(m_method);
  printf("Result code: %s\n", SHOW(m_method->get_code()));
  m_method->get_code()->build_cfg(/* editable */ false);

  printf("Result cfg: %s\n", SHOW(m_method->get_code()->cfg()));

  EXPECT_EQ(
      17,
      std::distance(m_method->get_code()->begin(), m_method->get_code()->end()))
      << show(code);
}
