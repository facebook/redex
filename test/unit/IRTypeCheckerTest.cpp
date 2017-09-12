/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

#include "DexAsm.h"
#include "DexClass.h"
#include "DexOpcode.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRTypeChecker.h"
#include "LocalDce.h"
#include "RedexContext.h"

class IRTypeCheckerTest : public ::testing::Test {
 public:
  ~IRTypeCheckerTest() { delete g_redex; }

  IRTypeCheckerTest() {
    g_redex = new RedexContext();
    auto args = DexTypeList::make_type_list({
        DexType::make_type("I"), // v10
        DexType::make_type("B"), // v11
        DexType::make_type("J"), // v12/v13
        DexType::make_type("Z"), // v14
        DexType::make_type("D"), // v15/v16
        DexType::make_type("S"), // v17
        DexType::make_type("F"), // v18
        get_object_type() // v19
    });
    auto proto = DexProto::make_proto(get_boolean_type(), args);
    m_method = static_cast<DexMethod*>(
        DexMethod::make_method(DexType::make_type("Lbar;"),
                               DexString::make_string("testMethod"),
                               proto));
    m_method->set_deobfuscated_name("testMethod");
    m_method->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);
    m_method->set_code(std::make_unique<IRCode>(m_method, /* temp_regs */ 10));
  }

  void add_code(const std::vector<IRInstruction*>& insns) {
    IRCode* code = m_method->get_code();
    for (const auto& insn : insns) {
      code->push_back(insn);
    }
  }

 protected:
  DexMethod* m_method;
};

TEST_F(IRTypeCheckerTest, arrayRead) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CHECK_CAST, DexType::make_type("[I"), {0_v, 19_v}),
      dasm(OPCODE_AGET, {1_v, 0_v, 10_v}),
      dasm(OPCODE_ADD_INT, {2_v, 1_v, 10_v}),
      dasm(OPCODE_RETURN, {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  EXPECT_TRUE(checker.good());
  EXPECT_EQ("OK", checker.what());
  EXPECT_EQ(SCALAR, checker.get_type(insns[2], 1));
  EXPECT_EQ(INT, checker.get_type(insns[3], 1));
}

TEST_F(IRTypeCheckerTest, arrayReadWide) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CHECK_CAST, DexType::make_type("[D"), {0_v, 19_v}),
      dasm(OPCODE_AGET_WIDE, {1_v, 0_v, 10_v}),
      dasm(OPCODE_ADD_DOUBLE, {3_v, 1_v, 15_v}),
      dasm(OPCODE_RETURN, {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  EXPECT_TRUE(checker.good());
  EXPECT_EQ(SCALAR1, checker.get_type(insns[2], 1));
  EXPECT_EQ(SCALAR2, checker.get_type(insns[2], 2));
  EXPECT_EQ(DOUBLE1, checker.get_type(insns[3], 3));
  EXPECT_EQ(DOUBLE2, checker.get_type(insns[3], 4));
}

TEST_F(IRTypeCheckerTest, multipleDefinitions) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CHECK_CAST, DexType::make_type("[I"), {0_v, 19_v}),
      dasm(OPCODE_AGET, {0_v, 0_v, 10_v}),
      dasm(OPCODE_INT_TO_FLOAT, {0_v, 0_v}),
      dasm(OPCODE_NEG_FLOAT, {0_v, 0_v}),
      dasm(OPCODE_MOVE_OBJECT, {0_v, 19_v}),
      dasm(OPCODE_CHECK_CAST, DexType::make_type("Lfoo;"), {0_v, 0_v}),
      dasm(OPCODE_INVOKE_VIRTUAL,
           DexMethod::make_method("LFoo;", "bar", "J", {"S"}),
           {0_v, 17_v}),
      dasm(OPCODE_MOVE_RESULT_WIDE, {0_v}),
      dasm(OPCODE_RETURN, {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  EXPECT_TRUE(checker.good());
  EXPECT_EQ(REFERENCE, checker.get_type(insns[1], 0));
  EXPECT_EQ(SCALAR, checker.get_type(insns[2], 0));
  EXPECT_EQ(FLOAT, checker.get_type(insns[3], 0));
  EXPECT_EQ(FLOAT, checker.get_type(insns[4], 0));
  EXPECT_EQ(REFERENCE, checker.get_type(insns[5], 0));
  EXPECT_EQ(REFERENCE, checker.get_type(insns[6], 0));
  EXPECT_EQ(LONG1, checker.get_type(insns[8], 0));
  EXPECT_EQ(LONG2, checker.get_type(insns[8], 1));
}

TEST_F(IRTypeCheckerTest, referenceFromInteger) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_MOVE, {0_v, 10_v}),
      dasm(OPCODE_AGET, {0_v, 0_v, 10_v}),
      dasm(OPCODE_RETURN, {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'AGET v0, v0, v10' for "
      "register v0: expected type REFERENCE, but found INT instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, misalignedLong) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_MOVE_WIDE, {0_v, 12_v}),
      dasm(OPCODE_NEG_LONG, {1_v, 1_v}),
      dasm(OPCODE_RETURN, {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'NEG_LONG v1, v1' for "
      "register v1: expected type (LONG1, LONG2), but found (LONG2, TOP) "
      "instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, uninitializedRegister) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_INVOKE_VIRTUAL,
           DexMethod::make_method("Lbar;", "foo", "V", {}),
           {0_v}),
      dasm(OPCODE_RETURN, {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'INVOKE_VIRTUAL v0, "
      "Lbar;.foo:()V' for register v0: expected type REFERENCE, but found TOP "
      "instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, undefinedRegister) {
  using namespace dex_asm;
  auto target1 = new BranchTarget();
  auto target2 = new BranchTarget();
  auto if_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {14_v}));
  target1->type = BRANCH_SIMPLE;
  target1->src = if_mie;
  auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO, {}));
  target2->type = BRANCH_SIMPLE;
  target2->src = goto_mie;
  IRCode* code = m_method->get_code();
  code->push_back(*if_mie); // branch to target1
  code->push_back(dasm(OPCODE_MOVE_OBJECT, {0_v, 19_v}));
  code->push_back(
      dasm(OPCODE_CHECK_CAST, DexType::make_type("Lbar;"), {0_v, 0_v}));
  code->push_back(*goto_mie); // branch to target2
  code->push_back(target1);
  code->push_back(dasm(OPCODE_MOVE, {0_v, 17_v}));
  code->push_back(target2);
  // Coming out of one branch, v0 is a reference and coming out of the other,
  // it's an integer.
  code->push_back(dasm(OPCODE_INVOKE_VIRTUAL,
                       DexMethod::make_method("Lbar;", "foo", "V", {}),
                       {0_v}));
  code->push_back(dasm(OPCODE_RETURN, {14_v}));
  IRTypeChecker checker(m_method);
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'INVOKE_VIRTUAL v0, "
      "Lbar;.foo:()V' for register v0: expected type REFERENCE, but found TOP "
      "instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, signatureMismatch) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CHECK_CAST, DexType::make_type("Lbar;"), {0_v, 19_v}),
      dasm(OPCODE_INVOKE_VIRTUAL,
           DexMethod::make_method("Lbar;", "foo", "V", {"I", "J", "Z"}),
           {0_v, 10_v, 12_v, 13_v, 18_v}),
      dasm(OPCODE_RETURN, {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'INVOKE_VIRTUAL v0, v10, "
      "v12, v18, Lbar;.foo:(IJZ)V' for register v18: expected type INT, but "
      "found FLOAT instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, comparisonOperation) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_MOVE_WIDE, {0_v, 15_v}),
      dasm(OPCODE_CMP_LONG, {0_v, 12_v, 0_v}),
      dasm(OPCODE_RETURN, {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'CMP_LONG v0, v12, v0' "
      "for register v0: expected type (LONG1, LONG2), but found (DOUBLE1, "
      "DOUBLE2) instead",
      checker.what());
}
