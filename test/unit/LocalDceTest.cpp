/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexUtil.h"
#include "InstructionLowering.h"
#include "IRCode.h"
#include "LocalDce.h"

struct LocalDceTryTest : testing::Test {
  DexMethod* m_method;

  LocalDceTryTest() {
    g_redex = new RedexContext();
    auto args = DexTypeList::make_type_list({});
    auto proto = DexProto::make_proto(get_void_type(), args);
    m_method = static_cast<DexMethod*>(DexMethod::make_method(
        get_object_type(), DexString::make_string("testMethod"), proto));
    m_method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
    m_method->set_code(std::make_unique<IRCode>(m_method, 1));
  }

  ~LocalDceTryTest() {
    delete g_redex;
  }
};

// We used to wrongly delete try items when just one of the the TRY_START /
// TRY_END markers was inside an unreachable block. We would remove both
// markers even though it was still bracketing live code. This test
// checks to see that we preserve the TRY markers while removing the
// relevant dead code.
TEST_F(LocalDceTryTest, deadCodeAfterTry) {
  // setup
  using namespace dex_asm;

  auto code = m_method->get_code();
  auto exception_type = DexType::make_type("Ljava/lang/Exception;");
  auto catch_start = new MethodItemEntry(exception_type);

  auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO));
  auto target = new BranchTarget(goto_mie);

  code->push_back(target);
  // this TRY_START is in a block that is live
  code->push_back(TRY_START, catch_start);
  // this invoke will be considered live code by the dce analysis
  code->push_back(dasm(OPCODE_INVOKE_STATIC, m_method, {}));
  code->push_back(*goto_mie);
  // this TRY_END is in a block that is dead code
  code->push_back(TRY_END, catch_start);
  code->push_back(dasm(OPCODE_INVOKE_STATIC, m_method, {}));
  code->push_back(*catch_start);
  code->push_back(dasm(OPCODE_RETURN_VOID));
  code->set_registers_size(0);

  LocalDcePass().run(code);
  instruction_lowering::lower(m_method);
  m_method->sync();

  // check that the dead const/16 opcode is removed, but that the try item
  // is preserved
  EXPECT_EQ(m_method->get_dex_code()->get_instructions().size(), 3);
  EXPECT_EQ(m_method->get_dex_code()->get_tries().size(), 1);
}

// Check that we correctly delete try blocks if all the code they are
// bracketing is unreachable
TEST_F(LocalDceTryTest, unreachableTry) {
  // setup
  using namespace dex_asm;

  auto code = m_method->get_code();
  auto exception_type = DexType::make_type("Ljava/lang/Exception;");
  auto catch_start = new MethodItemEntry(exception_type);

  auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO));
  auto target = new BranchTarget(goto_mie);

  code->push_back(target);
  code->push_back(dasm(OPCODE_INVOKE_STATIC, m_method, {}));
  code->push_back(*goto_mie);
  // everything onwards is unreachable code because of the goto

  code->push_back(TRY_START, catch_start);
  code->push_back(dasm(OPCODE_INVOKE_STATIC, m_method, {}));
  code->push_back(TRY_END, catch_start);
  code->push_back(*catch_start);
  code->push_back(dasm(OPCODE_INVOKE_STATIC, m_method, {}));
  code->set_registers_size(0);

  LocalDcePass().run(code);
  instruction_lowering::lower(m_method);
  m_method->sync();

  EXPECT_EQ(m_method->get_dex_code()->get_instructions().size(), 2);
  EXPECT_EQ(m_method->get_dex_code()->get_tries().size(), 0);
}

/*
 * Check that if a try block contains no throwing opcodes, we remove it
 * entirely, as well as the catch that it was supposed to throw to.
 *
 * Note that if a catch block at the end of a method is removed, it is
 * necessary to remove any tries that formerly targeted it, as catch target
 * offsets that point beyond the end of a method are a verification error.
 */
TEST_F(LocalDceTryTest, deadCatch) {
  // setup
  using namespace dex_asm;

  auto code = m_method->get_code();
  auto exception_type = DexType::make_type("Ljava/lang/Exception;");
  auto catch_start = new MethodItemEntry(exception_type);

  code->push_back(TRY_START, catch_start);
  code->push_back(dasm(OPCODE_RETURN_VOID));
  code->push_back(TRY_END, catch_start);
  code->push_back(*catch_start);
  code->push_back(dasm(OPCODE_INVOKE_STATIC, m_method, {}));
  code->set_registers_size(0);

  LocalDcePass().run(code);
  instruction_lowering::lower(m_method);
  m_method->sync();

  EXPECT_EQ(m_method->get_dex_code()->get_instructions().size(), 1);
  EXPECT_EQ(m_method->get_dex_code()->get_tries().size(), 0);
}

/*
 * Check that if a try block contains no throwing opcodes, we remove it
 * entirely, even if there are other blocks keeping its target catch live.
 */
TEST_F(LocalDceTryTest, tryNeverThrows) {
  // setup
  using namespace dex_asm;

  auto code = m_method->get_code();
  auto exception_type = DexType::make_type("Ljava/lang/Exception;");
  auto catch_start = new MethodItemEntry(exception_type);

  // this try wraps an opcode which may throw, should not be removed
  code->push_back(TRY_START, catch_start);
  code->push_back(dasm(OPCODE_INVOKE_STATIC, m_method, {}));
  code->push_back(TRY_END, catch_start);
  // this one doesn't wrap a may-throw opcode
  code->push_back(TRY_START, catch_start);
  code->push_back(dasm(OPCODE_CONST, {0_v, 0_L}));
  code->push_back(TRY_END, catch_start);
  code->push_back(dasm(OPCODE_INVOKE_STATIC, m_method, {}));
  code->push_back(*catch_start);
  code->push_back(dasm(OPCODE_RETURN_VOID));
  code->set_registers_size(1);

  LocalDcePass().run(code);
  instruction_lowering::lower(m_method);
  m_method->sync();

  EXPECT_EQ(m_method->get_dex_code()->get_instructions().size(), 3);
  EXPECT_EQ(m_method->get_dex_code()->get_tries().size(), 1);
}

TEST_F(LocalDceTryTest, deadIf) {
  // setup
  using namespace dex_asm;

  auto if_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {0_v}));
  auto target1 = new BranchTarget(if_mie);
  IRCode* code = m_method->get_code();
  code->push_back(*if_mie); // branch to target1
  code->push_back(target1);
  code->push_back(dasm(OPCODE_RETURN_VOID));
  code->set_registers_size(1);

  fprintf(stderr, "BEFORE:\n%s\n", SHOW(code));
  LocalDcePass().run(code);
  auto has_if =
      std::find_if(code->begin(), code->end(), [if_mie](MethodItemEntry& mie) {
        return &mie == if_mie;
      }) != code->end();

  // the if should be gone
  EXPECT_FALSE(has_if);
}
