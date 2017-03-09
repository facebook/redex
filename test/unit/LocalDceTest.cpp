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
#include "DexUtil.h"
#include "LocalDce.h"
#include "Transform.h"

struct LocalDceTryTest : testing::Test {
  DexMethod* m_method;

  LocalDceTryTest() {
    g_redex = new RedexContext();
    auto args = DexTypeList::make_type_list({});
    auto proto = DexProto::make_proto(get_void_type(), args);
    m_method = DexMethod::make_method(
        get_object_type(), DexString::make_string("testMethod"), proto);
    m_method->make_concrete(
        ACC_PUBLIC | ACC_STATIC, std::make_unique<DexCode>(), false);
    m_method->get_code()->set_registers_size(1);
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
  {
    using namespace dex_asm;

    MethodTransformer mt(m_method);
    auto exception_type = DexType::make_type("Ljava/lang/Exception;");
    auto catch_start = new MethodItemEntry(exception_type);

    auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO));
    auto target = new BranchTarget();
    target->type = BRANCH_SIMPLE;
    target->src = goto_mie;

    mt->push_back(target);
    // this TRY_START is in a block that is live
    mt->push_back(TRY_START, catch_start);
    // this invoke will be considered live code by the dce analysis
    mt->push_back(new DexOpcodeMethod(OPCODE_INVOKE_STATIC, m_method, 0));
    mt->push_back(*goto_mie);
    // this TRY_END is in a block that is dead code
    mt->push_back(TRY_END, catch_start);
    mt->push_back(dasm(OPCODE_CONST_16, {0_v, 0x1_L}));
    mt->push_back(*catch_start);
    mt->push_back(new DexOpcodeMethod(OPCODE_INVOKE_STATIC, m_method, 0));
  }

  EXPECT_EQ(m_method->get_code()->get_instructions().size(), 4);
  EXPECT_EQ(m_method->get_code()->get_tries().size(), 1);

  {
    MethodTransformer mt(m_method);
    LocalDcePass().run(m_method);
  }

  // check that the dead const/16 opcode is removed, but that the try item
  // is preserved
  EXPECT_EQ(m_method->get_code()->get_instructions().size(), 3);
  EXPECT_EQ(m_method->get_code()->get_tries().size(), 1);
}

// Check that we correctly delete try blocks if all the code they are
// bracketing is unreachable
TEST_F(LocalDceTryTest, unreachableTry) {
  // setup
  {
    using namespace dex_asm;

    MethodTransformer mt(m_method);
    auto exception_type = DexType::make_type("Ljava/lang/Exception;");
    auto catch_start = new MethodItemEntry(exception_type);

    auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO));
    auto target = new BranchTarget();
    target->type = BRANCH_SIMPLE;
    target->src = goto_mie;

    mt->push_back(target);
    mt->push_back(new DexOpcodeMethod(OPCODE_INVOKE_STATIC, m_method, 0));
    mt->push_back(*goto_mie);
    // everything onwards is unreachable code because of the goto

    mt->push_back(TRY_START, catch_start);
    mt->push_back(new DexOpcodeMethod(OPCODE_INVOKE_STATIC, m_method, 0));
    mt->push_back(TRY_END, catch_start);
    mt->push_back(*catch_start);
    mt->push_back(new DexOpcodeMethod(OPCODE_INVOKE_STATIC, m_method, 0));
  }

  EXPECT_EQ(m_method->get_code()->get_instructions().size(), 4);
  EXPECT_EQ(m_method->get_code()->get_tries().size(), 1);

  {
    MethodTransformer mt(m_method);
    LocalDcePass().run(m_method);
  }

  EXPECT_EQ(m_method->get_code()->get_instructions().size(), 2);
  EXPECT_EQ(m_method->get_code()->get_tries().size(), 0);
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
  {
    using namespace dex_asm;

    MethodTransformer mt(m_method);
    auto exception_type = DexType::make_type("Ljava/lang/Exception;");
    auto catch_start = new MethodItemEntry(exception_type);

    mt->push_back(TRY_START, catch_start);
    mt->push_back(dasm(OPCODE_RETURN_VOID));
    mt->push_back(TRY_END, catch_start);
    mt->push_back(*catch_start);
    mt->push_back(new DexOpcodeMethod(OPCODE_INVOKE_STATIC, m_method, 0));
  }
  EXPECT_EQ(m_method->get_code()->get_instructions().size(), 2);
  EXPECT_EQ(m_method->get_code()->get_tries().size(), 1);

  {
    MethodTransformer mt(m_method);
    LocalDcePass().run(m_method);
  }

  EXPECT_EQ(m_method->get_code()->get_instructions().size(), 1);
  EXPECT_EQ(m_method->get_code()->get_tries().size(), 0);
}

/*
 * Check that if a try block contains no throwing opcodes, we remove it
 * entirely, even if there are other blocks keeping its target catch live.
 */
TEST_F(LocalDceTryTest, tryNeverThrows) {
  // setup
  {
    using namespace dex_asm;

    MethodTransformer mt(m_method);
    auto exception_type = DexType::make_type("Ljava/lang/Exception;");
    auto catch_start = new MethodItemEntry(exception_type);

    // this try wraps an opcode which may throw, should not be removed
    mt->push_back(TRY_START, catch_start);
    mt->push_back(new DexOpcodeMethod(OPCODE_INVOKE_STATIC, m_method, 0));
    mt->push_back(TRY_END, catch_start);
    // this one doesn't wrap a may-throw opcode
    mt->push_back(TRY_START, catch_start);
    mt->push_back(dasm(OPCODE_RETURN_VOID));
    mt->push_back(TRY_END, catch_start);
    mt->push_back(*catch_start);
    mt->push_back(new DexOpcodeMethod(OPCODE_INVOKE_STATIC, m_method, 0));
  }
  EXPECT_EQ(m_method->get_code()->get_instructions().size(), 3);
  EXPECT_EQ(m_method->get_code()->get_tries().size(), 2);

  {
    MethodTransformer mt(m_method);
    LocalDcePass().run(m_method);
  }

  EXPECT_EQ(m_method->get_code()->get_instructions().size(), 3);
  EXPECT_EQ(m_method->get_code()->get_tries().size(), 1);
}
