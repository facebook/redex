/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>
#include <iterator>

#include "ControlFlow.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "RemoveGotos.h"

struct RemoveGotosTest : testing::Test {
  DexMethod* m_method;

  RemoveGotosTest() {
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

  void clear_method_code() {
    m_method->set_code(std::make_unique<IRCode>(m_method, 1));
  }

  ~RemoveGotosTest() { delete g_redex; }
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

  RemoveGotosPass().run(m_method);
  printf("Result code: %s\n", SHOW(m_method->get_code()));

  m_method->get_code()->build_cfg();
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

  m_method->get_code()->build_cfg();
  EXPECT_EQ(2, m_method->get_code()->cfg().blocks().size());

  RemoveGotosPass().run(m_method);
  printf("Result code: %s\n", SHOW(m_method->get_code()));

  m_method->get_code()->build_cfg();
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

  m_method->get_code()->build_cfg();
  EXPECT_EQ(3, m_method->get_code()->cfg().blocks().size());

  RemoveGotosPass().run(m_method);
  printf("Result code: %s\n", SHOW(m_method->get_code()));

  m_method->get_code()->build_cfg();
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
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 2_v, 2_v}));
  code->push_back(*if_mie);
  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 2_v, 2_v}));
  code->push_back(target);

  RemoveGotosPass().run(m_method);

  EXPECT_EQ(4, std::distance(code->begin(), code->end()));
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

/*
 * Considering blocks A ..... B C
 * where A -> B is a goto and B -> C is implicit (it just falls through)
 * moving B into A will break the CFG flow because the implicit link is broken.
 * therefore, even though in the CFG it appears to be able to be combined,
 * we shouldn't move them (without first applying some extra transformations)
 *
 * This appeared with a target falling through to a try catch block.
 */
TEST_F(RemoveGotosTest, excludeImplicitLinks) {
  using namespace dex_asm;
  clear_method_code();
  auto code = m_method->get_code();
  auto exception_type = DexType::make_type("Ljava/lang/Exception;");
  auto catch_start = new MethodItemEntry(exception_type);
  auto gt = create_goto();

  code->push_back(dasm(OPCODE_ADD_INT, {0_v, 0_v, 0_v}));
  code->push_back(*gt.first);

  code->push_back(dasm(OPCODE_MOVE, {0_v, 0_v}));
  code->push_back(gt.second);

  code->push_back(TRY_START, catch_start);
  code->push_back(dasm(OPCODE_DIV_DOUBLE, {0_v, 0_v, 1_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));
  code->push_back(TRY_END, catch_start);

  code->build_cfg();
  printf("Initial cfg: %s\n", SHOW(code->cfg()));

  auto num_goto_removed = RemoveGotosPass().run(m_method);

  code->build_cfg();
  printf("Final cfg: %s\n", SHOW(code->cfg()));

  EXPECT_EQ(5, code->cfg().blocks().size());
  EXPECT_EQ(5, code->count_opcodes());
  EXPECT_EQ(0, num_goto_removed);
}
