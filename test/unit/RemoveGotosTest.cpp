/*
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
#include "RedexTest.h"
#include "RemoveGotos.h"
#include "Show.h"

struct RemoveGotosTest : public RedexTest {
  DexMethod* m_method;

  RemoveGotosTest() {
    auto args = DexTypeList::make_type_list({});
    auto proto = DexProto::make_proto(type::_void(), args);
    m_method =
        DexMethod::make_method(type::java_lang_Object(),
                               DexString::make_string("testMethod"), proto)
            ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    m_method->set_code(std::make_unique<IRCode>(m_method, 1));
  }

  std::pair<MethodItemEntry*, BranchTarget*> create_goto() {
    using namespace dex_asm;

    auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO));
    auto target = new BranchTarget(goto_mie);
    return {goto_mie, target};
  }

  void clear_method_code() {
    m_method->set_code(std::make_unique<IRCode>(m_method, 1));
  }

  ~RemoveGotosTest() {}
};

// Code:    A B C D
// CFG:     A -> D -> C -> B
// Result:  ADCB
TEST_F(RemoveGotosTest, simplifySinglePath) {
  using namespace dex_asm;
  clear_method_code();

  auto gt1 = create_goto();
  auto gt2 = create_goto();
  auto gt3 = create_goto();

  auto code = m_method->get_code();
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 0_v, 0_v}));
  code->push_back(*gt1.first);

  code->push_back(gt3.second);
  code->push_back(dasm(OPCODE_ADD_INT, {3_v, 3_v, 3_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->push_back(gt2.second);
  code->push_back(dasm(OPCODE_ADD_INT, {2_v, 2_v, 2_v}));
  code->push_back(*gt3.first);

  code->push_back(gt1.second);
  code->push_back(dasm(OPCODE_ADD_INT, {1_v, 1_v, 1_v}));
  code->push_back(*gt2.first);
  code->set_registers_size(4);

  RemoveGotosPass().run(m_method);
  printf("Result code: %s\n", SHOW(m_method->get_code()));

  m_method->get_code()->build_cfg(/* editable */ false);
  EXPECT_EQ(1, m_method->get_code()->cfg().blocks().size());

  auto ins = InstructionIterable(m_method->get_code());
  auto iter = ins.begin();
  for (auto i = 0; i < 4; ++i) {
    EXPECT_EQ(iter->insn->opcode(), OPCODE_ADD_INT);
    if (iter->insn->opcode() == OPCODE_ADD_INT) {
      EXPECT_EQ(iter->insn->dest(), i);
      iter++;
    }
  }
  EXPECT_EQ(iter->insn->opcode(), OPCODE_RETURN_VOID);
  EXPECT_EQ(ins.end(), ++iter);
}

// Code:    A B
// CFG:     A -> B (with goto)
// Result:  AB
TEST_F(RemoveGotosTest, simplifyForwardsGoto) {
  using namespace dex_asm;
  clear_method_code();

  auto gt = create_goto();

  auto code = m_method->get_code();
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 0_v, 0_v}));
  code->push_back(*gt.first);

  code->push_back(gt.second);
  code->push_back(dasm(OPCODE_ADD_INT, {2_v, 2_v, 2_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));
  code->set_registers_size(3);

  m_method->get_code()->build_cfg(/* editable */ false);
  EXPECT_EQ(2, m_method->get_code()->cfg().blocks().size());

  RemoveGotosPass().run(m_method);
  printf("Result code: %s\n", SHOW(m_method->get_code()));

  m_method->get_code()->build_cfg(/* editable */ false);
  EXPECT_EQ(1, m_method->get_code()->cfg().blocks().size());
  EXPECT_EQ(3, m_method->get_code()->count_opcodes());
}

// Code:    A B C
// CFG:     A -> C -> B
// Result:  ACB
TEST_F(RemoveGotosTest, simplifyBackwardsGoto) {
  using namespace dex_asm;
  clear_method_code();

  auto gt1 = create_goto();
  auto gt2 = create_goto();

  auto code = m_method->get_code();
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 0_v, 0_v}));
  code->push_back(*gt1.first);

  code->push_back(gt2.second);
  code->push_back(dasm(OPCODE_ADD_INT, {2_v, 2_v, 2_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->push_back(gt1.second);
  code->push_back(dasm(OPCODE_ADD_INT, {1_v, 1_v, 1_v}));
  code->push_back(*gt2.first);
  code->set_registers_size(3);

  m_method->get_code()->build_cfg(/* editable */ false);
  EXPECT_EQ(3, m_method->get_code()->cfg().blocks().size());

  RemoveGotosPass().run(m_method);
  printf("Result code: %s\n", SHOW(m_method->get_code()));

  m_method->get_code()->build_cfg(/* editable */ false);
  auto iter = m_method->get_code()->begin();
  for (auto i = 0; i < 3; ++i) {
    EXPECT_EQ(iter->insn->opcode(), OPCODE_ADD_INT);
    if (iter->insn->opcode() == OPCODE_ADD_INT) {
      EXPECT_EQ(iter->insn->dest(), i);
      iter++;
    }
  }
  EXPECT_EQ(iter->insn->opcode(), OPCODE_RETURN_VOID);
  EXPECT_EQ(1, m_method->get_code()->cfg().blocks().size());
  EXPECT_EQ(4, m_method->get_code()->count_opcodes());
}

// Code:    A B C
// CFG:     A -> B and A -> C
// Result:  Keep same
TEST_F(RemoveGotosTest, skipSimpleBranch) {
  using namespace dex_asm;
  clear_method_code();

  auto code = m_method->get_code();
  auto if_mie = new MethodItemEntry(dasm(OPCODE_IF_EQ, {0_v, 1_v}));
  auto target = new BranchTarget(if_mie);
  code->push_back(dasm(IOPCODE_LOAD_PARAM, {0_v}));
  code->push_back(dasm(IOPCODE_LOAD_PARAM, {1_v}));
  code->push_back(dasm(IOPCODE_LOAD_PARAM, {2_v}));
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 2_v, 2_v}));
  code->push_back(*if_mie);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 2_v, 2_v}));
  code->push_back(target);
  code->push_back(dasm(OPCODE_RETURN_VOID));
  code->set_registers_size(3);

  RemoveGotosPass().run(m_method);

  EXPECT_EQ(8, std::distance(code->begin(), code->end())) << show(code);
}

// Code:    ABC
// CFG:     ABC
// Result:  Keep same
TEST_F(RemoveGotosTest, preserveSimplifiedMethod) {
  using namespace dex_asm;
  clear_method_code();

  auto code = m_method->get_code();
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 2_v, 2_v}));
  code->push_back(dasm(OPCODE_ADD_INT, {1_v, 2_v, 2_v}));
  code->push_back(dasm(OPCODE_ADD_INT, {2_v, 2_v, 2_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));
  code->set_registers_size(3);

  RemoveGotosPass().run(m_method);

  auto ins = InstructionIterable(m_method->get_code());
  auto iter = ins.begin();
  for (auto i = 0; i < 3; ++i) {
    EXPECT_EQ(iter->insn->opcode(), OPCODE_ADD_INT);
    if (iter->insn->opcode() == OPCODE_ADD_INT) {
      EXPECT_EQ(iter->insn->dest(), i);
      iter++;
    }
  }
  EXPECT_EQ(iter->insn->opcode(), OPCODE_RETURN_VOID);
  EXPECT_EQ(ins.end(), ++iter);
}
