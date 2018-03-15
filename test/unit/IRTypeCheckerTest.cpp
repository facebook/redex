/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

#include "DexAsm.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "IRTypeChecker.h"
#include "LocalDce.h"
#include "RedexContext.h"

using namespace testing;

class IRTypeCheckerTest : public ::testing::Test {
 public:
  ~IRTypeCheckerTest() { delete g_redex; }

  IRTypeCheckerTest() {
    g_redex = new RedexContext();
    auto args = DexTypeList::make_type_list({
        DexType::make_type("I"), // v5
        DexType::make_type("B"), // v6
        DexType::make_type("J"), // v7/v8
        DexType::make_type("Z"), // v9
        DexType::make_type("D"), // v10/v11
        DexType::make_type("S"), // v12
        DexType::make_type("F"), // v13
        get_object_type() // v14
    });
    auto proto = DexProto::make_proto(get_boolean_type(), args);
    m_method = static_cast<DexMethod*>(
        DexMethod::make_method(DexType::make_type("Lbar;"),
                               DexString::make_string("testMethod"),
                               proto));
    m_method->set_deobfuscated_name("testMethod");
    m_method->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);
    m_method->set_code(std::make_unique<IRCode>(m_method, /* temp_regs */ 5));
  }

  void add_code(const std::vector<IRInstruction*>& insns) {
    IRCode* code = m_method->get_code();
    for (const auto& insn : insns) {
      code->push_back(insn);
    }
  }

  void add_code(const std::unique_ptr<IRCode>& insns) {
    IRCode* code = m_method->get_code();
    for (const auto& insn : *insns) {
      code->push_back(insn);
    }
  }

 protected:
  DexMethod* m_method;
};

TEST_F(IRTypeCheckerTest, load_param) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_ADD_INT, {5_v, 5_v, 6_v}),
      dasm(IOPCODE_LOAD_PARAM, {5_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_THAT(checker.what(),
              MatchesRegex("^Encountered [0x[0-9a-f]*] OPCODE: "
                           "IOPCODE_LOAD_PARAM v5 not at the start "
                           "of the method$"));
}

TEST_F(IRTypeCheckerTest, move_result) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_FILLED_NEW_ARRAY, DexType::make_type("I"))
          ->set_arg_word_count(1)
          ->set_src(0, 5),
      dasm(OPCODE_ADD_INT, {5_v, 5_v, 5_v}),
      dasm(OPCODE_MOVE_RESULT, {0_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_THAT(checker.what(),
              MatchesRegex("^Encountered [0x[0-9a-f]*] OPCODE: MOVE_RESULT v0 "
                           "without appropriate prefix instruction. Expected "
                           "invoke or filled-new-array, got "
                           "ADD_INT v5, v5, v5$"));
}

TEST_F(IRTypeCheckerTest, move_result_at_start) {
  using namespace dex_asm;
  // Construct a new method because we don't want any load-param opcodes in
  // this one
  auto args = DexTypeList::make_type_list({});
  auto proto = DexProto::make_proto(get_boolean_type(), args);
  auto method = static_cast<DexMethod*>(
      DexMethod::make_method(DexType::make_type("Lbar;"),
                             DexString::make_string("testMethod2"),
                             proto));
  method->set_deobfuscated_name("testMethod2");
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);
  method->set_code(std::make_unique<IRCode>(method, 0));

  IRCode* code = method->get_code();
  code->push_back(dasm(OPCODE_MOVE_RESULT, {0_v}));
  code->push_back(dasm(OPCODE_ADD_INT, {5_v, 5_v, 5_v}));

  IRTypeChecker checker(method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_THAT(checker.what(),
              MatchesRegex("^Encountered [0x[0-9a-f]*] OPCODE: MOVE_RESULT v0 "
                           "at start of the method$"));
}

TEST_F(IRTypeCheckerTest, move_result_pseudo_no_prefix) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(IOPCODE_MOVE_RESULT_PSEUDO, {0_v}),
      dasm(OPCODE_ADD_INT, {5_v, 5_v, 5_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_THAT(
      checker.what(),
      MatchesRegex(
          "^Encountered [0x[0-9a-f]*] OPCODE: IOPCODE_MOVE_RESULT_PSEUDO v0 "
          "without appropriate prefix instruction$"));
}

TEST_F(IRTypeCheckerTest, move_result_pseudo_no_suffix) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CHECK_CAST, get_object_type(), {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_THAT(checker.what(),
              MatchesRegex("^Did not find move-result-pseudo after "
                           "[0x[0-9a-f]*] OPCODE: CHECK_CAST v14, "
                           "Ljava/lang/Object;"));
}

TEST_F(IRTypeCheckerTest, arrayRead) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CHECK_CAST, DexType::make_type("[I"), {14_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_AGET, {0_v, 5_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO, {1_v}),
      dasm(OPCODE_ADD_INT, {2_v, 1_v, 5_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ("OK", checker.what());
  EXPECT_EQ(SCALAR, checker.get_type(insns[4], 1));
  EXPECT_EQ(INT, checker.get_type(insns[5], 1));
}

TEST_F(IRTypeCheckerTest, arrayReadWide) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CHECK_CAST, DexType::make_type("[D"), {14_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_AGET_WIDE, {0_v, 5_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_WIDE, {1_v}),
      dasm(OPCODE_ADD_DOUBLE, {3_v, 1_v, 10_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ(SCALAR1, checker.get_type(insns[4], 1));
  EXPECT_EQ(SCALAR2, checker.get_type(insns[4], 2));
  EXPECT_EQ(DOUBLE1, checker.get_type(insns[5], 3));
  EXPECT_EQ(DOUBLE2, checker.get_type(insns[5], 4));
}

TEST_F(IRTypeCheckerTest, multipleDefinitions) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CHECK_CAST, DexType::make_type("[I"), {14_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_AGET, {0_v, 5_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO, {0_v}),
      dasm(OPCODE_INT_TO_FLOAT, {0_v, 0_v}),
      dasm(OPCODE_NEG_FLOAT, {0_v, 0_v}),
      dasm(OPCODE_MOVE_OBJECT, {0_v, 14_v}),
      dasm(OPCODE_CHECK_CAST, DexType::make_type("Lfoo;"), {0_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_INVOKE_VIRTUAL,
           DexMethod::make_method("LFoo;", "bar", "J", {"S"}),
           {0_v, 12_v}),
      dasm(OPCODE_MOVE_RESULT_WIDE, {0_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ(REFERENCE, checker.get_type(insns[2], 0));
  EXPECT_EQ(SCALAR, checker.get_type(insns[4], 0));
  EXPECT_EQ(FLOAT, checker.get_type(insns[5], 0));
  EXPECT_EQ(FLOAT, checker.get_type(insns[6], 0));
  EXPECT_EQ(REFERENCE, checker.get_type(insns[7], 0));
  EXPECT_EQ(REFERENCE, checker.get_type(insns[9], 0));
  EXPECT_EQ(LONG1, checker.get_type(insns[11], 0));
  EXPECT_EQ(LONG2, checker.get_type(insns[11], 1));
}

TEST_F(IRTypeCheckerTest, referenceFromInteger) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_MOVE, {0_v, 5_v}),
      dasm(OPCODE_AGET, {0_v, 5_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO, {0_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'AGET v0, v5' for "
      "register v0: expected type REFERENCE, but found INT instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, misalignedLong) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_MOVE_WIDE, {0_v, 7_v}),
      dasm(OPCODE_NEG_LONG, {1_v, 1_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
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
      dasm(OPCODE_RETURN, {9_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'INVOKE_VIRTUAL v0, "
      "Lbar;.foo:()V' for register v0: expected type REFERENCE, but found TOP "
      "instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, undefinedRegister) {
  using namespace dex_asm;
  auto if_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {9_v}));
  auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO, {}));
  auto target1 = new BranchTarget(if_mie);
  auto target2 = new BranchTarget(goto_mie);
  IRCode* code = m_method->get_code();
  code->push_back(*if_mie); // branch to target1
  code->push_back(dasm(OPCODE_MOVE_OBJECT, {0_v, 14_v}));
  code->push_back(dasm(OPCODE_CHECK_CAST, DexType::make_type("Lbar;"), {0_v}));
  code->push_back(
		  dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}));
  code->push_back(*goto_mie); // branch to target2
  code->push_back(target1);
  code->push_back(dasm(OPCODE_MOVE, {0_v, 12_v}));
  code->push_back(target2);
  // Coming out of one branch, v0 is a reference and coming out of the other,
  // it's an integer.
  code->push_back(dasm(OPCODE_INVOKE_VIRTUAL,
                       DexMethod::make_method("Lbar;", "foo", "V", {}),
                       {0_v}));
  code->push_back(dasm(OPCODE_RETURN, {9_v}));
  IRTypeChecker checker(m_method);
  checker.run();
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
      dasm(OPCODE_CHECK_CAST, DexType::make_type("Lbar;"), {14_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_INVOKE_VIRTUAL,
           DexMethod::make_method("Lbar;", "foo", "V", {"I", "J", "Z"}),
           {0_v, 5_v, 7_v, 13_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'INVOKE_VIRTUAL v0, v5, "
      "v7, v13, Lbar;.foo:(IJZ)V' for register v13: expected type INT, but "
      "found FLOAT instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, longInvoke) {
  using namespace dex_asm;
  IRInstruction* invoke = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke->set_arg_word_count(7);
  invoke->set_method(DexMethod::make_method(
      "Lbar;", "foo", "V", {"I", "B", "J", "Z", "D", "S", "F"}));
  invoke->set_src(0, 5);
  invoke->set_src(1, 6);
  invoke->set_src(2, 7);
  invoke->set_src(3, 9);
  invoke->set_src(4, 10);
  invoke->set_src(5, 12);
  invoke->set_src(6, 13);
  std::vector<IRInstruction*> insns = {invoke, dasm(OPCODE_RETURN, {9_v})};
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.good()) << checker.what();
}

TEST_F(IRTypeCheckerTest, longSignatureMismatch) {
  using namespace dex_asm;
  IRInstruction* invoke = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke->set_arg_word_count(7);
  invoke->set_method(DexMethod::make_method(
      "Lbar;", "foo", "V", {"I", "B", "J", "Z", "S", "D", "F"}));
  invoke->set_src(0, 5);
  invoke->set_src(1, 6);
  invoke->set_src(2, 7);
  invoke->set_src(3, 9);
  invoke->set_src(4, 10);
  invoke->set_src(5, 11);
  invoke->set_src(6, 13);
  std::vector<IRInstruction*> insns = {invoke, dasm(OPCODE_RETURN, {9_v})};
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'INVOKE_STATIC "
      "v5, v6, v7, v9, v10, v11, v13, Lbar;.foo:(IBJZSDF)V' for "
      "register v10: expected type INT, but found DOUBLE1 instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, comparisonOperation) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_MOVE_WIDE, {0_v, 10_v}),
      dasm(OPCODE_CMP_LONG, {0_v, 7_v, 0_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'CMP_LONG v0, v7, v0' "
      "for register v0: expected type (LONG1, LONG2), but found (DOUBLE1, "
      "DOUBLE2) instead",
      checker.what());
}

TEST_F(IRTypeCheckerTest, verifyMoves) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_MOVE_OBJECT, {1_v, 0_v}),
      dasm(OPCODE_MOVE, {1_v, 9_v}),
      dasm(OPCODE_RETURN, {1_v}),
  };
  add_code(insns);
  IRTypeChecker lax_checker(m_method);
  lax_checker.run();
  EXPECT_TRUE(lax_checker.good()) << lax_checker.what();
  IRTypeChecker strict_checker(m_method);
  strict_checker.verify_moves();
  strict_checker.run();
  EXPECT_TRUE(strict_checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'MOVE_OBJECT v1, v0' for "
      "register v0: expected type REFERENCE, but found TOP instead",
      strict_checker.what());
}

TEST_F(IRTypeCheckerTest, exceptionHandler) {
  using namespace dex_asm;
  auto exception_type = DexType::make_type("Ljava/lang/Exception;");
  auto catch_start = new MethodItemEntry(exception_type);
  IRCode* code = m_method->get_code();
  IRInstruction* noexc_return = dasm(OPCODE_RETURN, {1_v});
  IRInstruction* exc_return = dasm(OPCODE_RETURN, {0_v});
  code->push_back(dasm(OPCODE_MOVE, {0_v, 9_v}));
  code->push_back(dasm(OPCODE_CONST, {1_v, 0_L}));
  code->push_back(dasm(OPCODE_CONST, {2_v, 12_L}));
  code->push_back(TRY_START, catch_start);
  code->push_back(dasm(OPCODE_DIV_INT, {5_v, 5_v})); // Can throw
  code->push_back(dasm(IOPCODE_MOVE_RESULT_PSEUDO, {2_v}));
  code->push_back(dasm(OPCODE_CONST, {1_v, 1_L}));
  code->push_back(dasm(OPCODE_MOVE, {3_v, 1_v}));
  code->push_back(TRY_END, catch_start);
  code->push_back(noexc_return);
  code->push_back(*catch_start);
  code->push_back(exc_return);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ(INT, checker.get_type(noexc_return, 0));
  EXPECT_EQ(CONST, checker.get_type(noexc_return, 1));
  EXPECT_EQ(INT, checker.get_type(noexc_return, 2));
  EXPECT_EQ(CONST, checker.get_type(noexc_return, 3));
  // The exception is thrown by DIV_INT before v2 is modified.
  EXPECT_EQ(INT, checker.get_type(exc_return, 0));
  EXPECT_EQ(ZERO, checker.get_type(exc_return, 1));
  EXPECT_EQ(CONST, checker.get_type(exc_return, 2));
  EXPECT_EQ(TOP, checker.get_type(exc_return, 3));
  EXPECT_EQ(INT, checker.get_type(exc_return, 5));
  // The rest of the type environment, like method parameters, should be
  // left unchanged in the exception handler.
  EXPECT_EQ(REFERENCE, checker.get_type(exc_return, 14));
}

TEST_F(IRTypeCheckerTest, polymorphicConstants1) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CONST, {0_v, 128_L}),
      dasm(OPCODE_ADD_INT, {5_v, 5_v, 0_v}),
      dasm(OPCODE_MUL_FLOAT, {13_v, 13_v, 0_v}),
  };
  add_code(insns);
  IRTypeChecker polymorphic_checker(m_method);
  polymorphic_checker.enable_polymorphic_constants();
  polymorphic_checker.run();
  EXPECT_TRUE(polymorphic_checker.good()) << polymorphic_checker.what();
  IRTypeChecker regular_checker(m_method);
  regular_checker.run();
  EXPECT_TRUE(regular_checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'MUL_FLOAT v13, "
      "v13, v0' for register v0: expected type FLOAT, but found INT instead",
      regular_checker.what());
}

TEST_F(IRTypeCheckerTest, polymorphicConstants2) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CONST_WIDE, {0_v, 128_L}),
      dasm(OPCODE_ADD_LONG, {7_v, 7_v, 0_v}),
      dasm(OPCODE_MUL_DOUBLE, {10_v, 10_v, 0_v}),
  };
  add_code(insns);
  IRTypeChecker polymorphic_checker(m_method);
  polymorphic_checker.enable_polymorphic_constants();
  polymorphic_checker.run();
  EXPECT_TRUE(polymorphic_checker.good()) << polymorphic_checker.what();
  IRTypeChecker regular_checker(m_method);
  regular_checker.run();
  EXPECT_TRUE(regular_checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'MUL_DOUBLE v10, "
      "v10, v0' for register v0: expected type (DOUBLE1, DOUBLE2), but found "
      "(LONG1, LONG2) instead",
      regular_checker.what());
}

TEST_F(IRTypeCheckerTest, polymorphicConstants3) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CONST, {0_v, 0_L}),
      dasm(OPCODE_AGET, {0_v, 5_v}),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO, {1_v}),
      dasm(OPCODE_ADD_INT, {5_v, 5_v, 0_v}),
  };
  add_code(insns);
  IRTypeChecker polymorphic_checker(m_method);
  polymorphic_checker.enable_polymorphic_constants();
  polymorphic_checker.run();
  EXPECT_TRUE(polymorphic_checker.good()) << polymorphic_checker.what();
  IRTypeChecker regular_checker(m_method);
  regular_checker.run();
  EXPECT_TRUE(regular_checker.fail());
  EXPECT_EQ(
      "Type error in method testMethod at instruction 'ADD_INT v5, v5, "
      "v0' for register v0: expected type INT, but found REFERENCE instead",
      regular_checker.what());
}

TEST_F(IRTypeCheckerTest, overlappingMoveWide) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_MOVE_WIDE, {1_v, 7_v}),
      dasm(OPCODE_MOVE_WIDE, {0_v, 1_v}),
      dasm(OPCODE_MOVE_WIDE, {0_v, 10_v}),
      dasm(OPCODE_MOVE_WIDE, {1_v, 0_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ("OK", checker.what());
  EXPECT_EQ(LONG1, checker.get_type(insns[1], 1));
  EXPECT_EQ(LONG2, checker.get_type(insns[1], 2));
  EXPECT_EQ(LONG1, checker.get_type(insns[2], 0));
  EXPECT_EQ(LONG2, checker.get_type(insns[2], 1));
  EXPECT_EQ(DOUBLE1, checker.get_type(insns[3], 0));
  EXPECT_EQ(DOUBLE2, checker.get_type(insns[3], 1));
  EXPECT_EQ(DOUBLE1, checker.get_type(insns[4], 1));
  EXPECT_EQ(DOUBLE2, checker.get_type(insns[4], 2));
}

TEST_F(IRTypeCheckerTest, filledNewArray) {
  auto insns = assembler::ircode_from_string(R"(
    (
      (const-string "S1")
      (move-result-pseudo-object v1)
      (const-string "S2")
      (move-result-pseudo-object v2)
      (const-string "S3")
      (move-result-pseudo-object v3)
      (filled-new-array (v1 v2 v3) "[Ljava/lang/String;")
      (move-result-object v0)
      (return v9)
    )
  )");
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ("OK", checker.what());
}
