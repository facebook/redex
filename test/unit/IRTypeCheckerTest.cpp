/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>

#include "Creators.h"
#include "DexAsm.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "IRTypeChecker.h"
#include "LocalDce.h"
#include "RedexTest.h"

using namespace testing;

class IRTypeCheckerTest : public RedexTest {
 public:
  ~IRTypeCheckerTest() {}

  IRTypeCheckerTest() {
    auto args = DexTypeList::make_type_list({
        DexType::make_type("I"), // v5
        DexType::make_type("B"), // v6
        DexType::make_type("J"), // v7/v8
        DexType::make_type("Z"), // v9
        DexType::make_type("D"), // v10/v11
        DexType::make_type("S"), // v12
        DexType::make_type("F"), // v13
        type::java_lang_Object() // v14
    });
    ClassCreator cc(type::java_lang_Object());
    cc.set_access(ACC_PUBLIC);
    cc.set_external();
    auto object_class = cc.create();

    auto proto = DexProto::make_proto(type::_boolean(), args);
    m_method =
        DexMethod::make_method(DexType::make_type("Lbar;"),
                               DexString::make_string("testMethod"),
                               proto)
            ->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);
    m_method->set_deobfuscated_name("testMethod");
    m_method->set_code(std::make_unique<IRCode>(m_method, /* temp_regs */ 5));

    proto = DexProto::make_proto(type::java_lang_Object(), args);
    m_method_ret_obj =
        DexMethod::make_method(DexType::make_type("Lbar;"),
                               DexString::make_string("testMethodRetObj"),
                               proto)
            ->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);
    m_method_ret_obj->set_deobfuscated_name("testMethodRetObj");
    m_method_ret_obj->set_code(
        std::make_unique<IRCode>(m_method_ret_obj, /* temp_regs */ 5));

    m_virtual_method =
        DexMethod::make_method(DexType::make_type("Lbar;"),
                               DexString::make_string("testVirtualMethod"),
                               proto)
            ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    m_virtual_method->set_deobfuscated_name("testVirtualMethod");
    m_virtual_method->set_code(
        std::make_unique<IRCode>(m_virtual_method, /* temp_regs */ 5));
  }

  void add_code(const std::vector<IRInstruction*>& insns) {
    add_code(m_method, insns);
  }

  void add_code(const std::unique_ptr<IRCode>& insns) {
    add_code(m_method, insns);
  }

  void add_code_ret_obj(const std::vector<IRInstruction*>& insns) {
    add_code(m_method_ret_obj, insns);
  }

  void add_code_ret_obj(const std::unique_ptr<IRCode>& insns) {
    add_code(m_method_ret_obj, insns);
  }

  void add_code(DexMethod* m, const std::vector<IRInstruction*>& insns) {
    IRCode* code = m->get_code();
    for (const auto& insn : insns) {
      code->push_back(insn);
    }
  }

  void add_code(DexMethod* m, const std::unique_ptr<IRCode>& insns) {
    IRCode* code = m->get_code();
    for (const auto& insn : *insns) {
      code->push_back(insn);
    }
  }

 protected:
  DexMethod* m_method;
  DexMethod* m_method_ret_obj;
  DexMethod* m_virtual_method;
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
              MatchesRegex("^Encountered [0x[0-9a-f]+] OPCODE: "
                           "IOPCODE_LOAD_PARAM v5 not at the start "
                           "of the method$"));
}

TEST_F(IRTypeCheckerTest, move_result) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_FILLED_NEW_ARRAY, DexType::make_type("I"))
          ->set_srcs_size(1)
          ->set_src(0, 5),
      dasm(OPCODE_ADD_INT, {5_v, 5_v, 5_v}),
      dasm(OPCODE_MOVE_RESULT, {0_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_THAT(checker.what(),
              MatchesRegex("^Encountered [0x[0-9a-f]+] OPCODE: MOVE_RESULT v0 "
                           "without appropriate prefix instruction. Expected "
                           "invoke or filled-new-array, got "
                           "ADD_INT v5, v5, v5$"));
}

TEST_F(IRTypeCheckerTest, move_result_at_start) {
  using namespace dex_asm;
  // Construct a new method because we don't want any load-param opcodes in
  // this one
  auto args = DexTypeList::make_type_list({});
  auto proto = DexProto::make_proto(type::_boolean(), args);
  auto method =
      DexMethod::make_method(DexType::make_type("Lbar;"),
                             DexString::make_string("testMethod2"),
                             proto)
          ->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);
  method->set_deobfuscated_name("testMethod2");
  method->set_code(std::make_unique<IRCode>(method, 0));

  IRCode* code = method->get_code();
  code->push_back(dasm(OPCODE_MOVE_RESULT, {0_v}));
  code->push_back(dasm(OPCODE_ADD_INT, {5_v, 5_v, 5_v}));

  IRTypeChecker checker(method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_THAT(checker.what(),
              MatchesRegex("^Encountered [0x[0-9a-f]+] OPCODE: MOVE_RESULT v0 "
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
          "^Encountered [0x[0-9a-f]+] OPCODE: IOPCODE_MOVE_RESULT_PSEUDO v0 "
          "without appropriate prefix instruction$"));
}

TEST_F(IRTypeCheckerTest, move_result_pseudo_no_suffix) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CHECK_CAST, type::java_lang_Object(), {14_v}),
  };
  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_THAT(checker.what(),
              ContainsRegex("^Did not find move-result-pseudo after "
                            "[0x[0-9a-f]+] OPCODE: CHECK_CAST v14, "
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
  EXPECT_THAT(
      checker.what(),
      MatchesRegex(
          "^Type error in method testMethod at instruction 'AGET v0, v5' "
          "@ 0x[0-9a-f]+ for register v0: expected type REF, but found "
          "INT instead"));
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
  EXPECT_THAT(
      checker.what(),
      MatchesRegex(
          "^Type error in method testMethod at instruction 'NEG_LONG v1, v1' "
          "@ 0x[0-9a-f]+ for register v1: expected type \\(LONG1, LONG2\\), "
          "but found \\(LONG2, TOP\\) instead"));
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
  EXPECT_THAT(
      checker.what(),
      MatchesRegex(
          "^Type error in method testMethod at instruction 'INVOKE_VIRTUAL v0, "
          "Lbar;\\.foo:\\(\\)V' @ 0x[0-9a-f]+ for register v0: expected "
          "type REF, but found TOP instead"));
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
  code->push_back(dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}));
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
  EXPECT_THAT(
      checker.what(),
      MatchesRegex(
          "^Type error in method testMethod at instruction 'INVOKE_VIRTUAL v0, "
          "Lbar;\\.foo:\\(\\)V' @ 0x[0-9a-f]+ for register v0: expected "
          "type REF, but found TOP instead"));
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
  EXPECT_THAT(
      checker.what(),
      MatchesRegex(
          "^Type error in method testMethod at instruction 'INVOKE_VIRTUAL v0, "
          "v5, v7, v13, Lbar;\\.foo:\\(IJZ\\)V' @ 0x[0-9a-f]+ for register "
          "v13: expected type INT, but found FLOAT instead"));
}

TEST_F(IRTypeCheckerTest, longInvoke) {
  using namespace dex_asm;
  IRInstruction* invoke = new IRInstruction(OPCODE_INVOKE_STATIC);
  invoke->set_srcs_size(7);
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
  invoke->set_srcs_size(7);
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
  EXPECT_THAT(
      checker.what(),
      MatchesRegex(
          "^Type error in method testMethod at instruction 'INVOKE_STATIC "
          "v5, v6, v7, v9, v10, v11, v13, Lbar;\\.foo:\\(IBJZSDF\\)V' "
          "@ 0x[0-9a-f]+ for register v10: expected type INT, but found "
          "DOUBLE1 "
          "instead"));
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
  EXPECT_THAT(
      checker.what(),
      MatchesRegex(
          "^Type error in method testMethod at instruction 'CMP_LONG v0, v7, "
          "v0' @ 0x[0-9a-f]+ for register v0: expected type \\(LONG1, "
          "LONG2\\), but found \\(DOUBLE1, DOUBLE2\\) instead"));
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
  EXPECT_THAT(
      strict_checker.what(),
      MatchesRegex(
          "^Type error in method testMethod at instruction "
          "'MOVE_OBJECT v1, v0' @ 0x[0-9a-f]+ for register v0: expected type "
          "REF, but found TOP instead"));
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

TEST_F(IRTypeCheckerTest, overlappingMoveWide) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_MOVE_WIDE, {1_v, 7_v}),  dasm(OPCODE_MOVE_WIDE, {0_v, 1_v}),
      dasm(OPCODE_MOVE_WIDE, {0_v, 10_v}), dasm(OPCODE_MOVE_WIDE, {1_v, 0_v}),
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

TEST_F(IRTypeCheckerTest, zeroOrReference) {
  using namespace dex_asm;
  std::vector<IRInstruction*> insns = {
      dasm(OPCODE_CONST_CLASS, DexType::make_type("Lbar;")),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_MONITOR_ENTER, {0_v}),
      dasm(OPCODE_CONST, {1_v, 0_L}),
      dasm(OPCODE_MONITOR_EXIT, {0_v}),
      dasm(OPCODE_RETURN_OBJECT, {0_v}),
      dasm(OPCODE_MOVE_EXCEPTION, {1_v}),
      dasm(OPCODE_MONITOR_EXIT, {0_v}),
      dasm(OPCODE_THROW, {1_v}),
  };
  add_code_ret_obj(insns);
  IRTypeChecker checker(m_method_ret_obj);
  checker.run();
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ("OK", checker.what());
  EXPECT_EQ(REFERENCE, checker.get_type(insns[2], 0));
  EXPECT_EQ(REFERENCE, checker.get_type(insns[3], 0));
  EXPECT_EQ(REFERENCE, checker.get_type(insns[4], 0));
  EXPECT_EQ(ZERO, checker.get_type(insns[4], 1));
  EXPECT_EQ(REFERENCE, checker.get_type(insns[5], 0));
  EXPECT_EQ(ZERO, checker.get_type(insns[5], 1));
  EXPECT_EQ(BOTTOM, checker.get_type(insns[6], 0));
  EXPECT_EQ(BOTTOM, checker.get_type(insns[6], 1));
  EXPECT_EQ(BOTTOM, checker.get_type(insns[7], 1));
  EXPECT_EQ(BOTTOM, checker.get_type(insns[8], 1));
}

/**
 * The bytecode stream of the following Java code.
 * A simple branch join scenario on a reference type.
 *
 * Base base = null;
 * if (condition) {
 *   base = new A();
 *   base.foo();
 * } else {
 *   base = new B();
 *   base.foo();
 * }
 * base.foo();
 */
TEST_F(IRTypeCheckerTest, joinDexTypesSharingCommonBaseSimple) {
  // Construct type hierarchy.
  const auto type_base = DexType::make_type("LBase;");
  const auto type_a = DexType::make_type("LA;");
  const auto type_b = DexType::make_type("LB;");

  ClassCreator cls_base_creator(type_base);
  cls_base_creator.set_super(type::java_lang_Object());
  auto base_foo =
      DexMethod::make_method("LBase;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  cls_base_creator.add_method(base_foo);
  cls_base_creator.create();

  ClassCreator cls_a_creator(type_a);
  cls_a_creator.set_super(type_base);
  auto a_ctor = DexMethod::make_method("LA;.<init>:()V")
                    ->make_concrete(ACC_PUBLIC, false);
  cls_a_creator.add_method(a_ctor);
  auto a_foo =
      DexMethod::make_method("LA;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  cls_a_creator.add_method(a_foo);
  cls_a_creator.create();

  ClassCreator cls_b_creator(type_b);
  cls_b_creator.set_super(type_base);
  auto b_ctor = DexMethod::make_method("LB;.<init>:()V")
                    ->make_concrete(ACC_PUBLIC, false);
  cls_b_creator.add_method(b_ctor);
  auto b_foo =
      DexMethod::make_method("LB;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  cls_b_creator.add_method(b_foo);
  cls_b_creator.create();

  // Construct code that references the above hierarchy.
  using namespace dex_asm;
  auto if_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {5_v}));
  auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO, {}));
  auto target1 = new BranchTarget(if_mie);
  auto target2 = new BranchTarget(goto_mie);

  std::vector<IRInstruction*> insns = {
      // B0
      // *if_mie, // branch to target1
      // B1
      dasm(OPCODE_NEW_INSTANCE, type_a),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_INVOKE_DIRECT, a_ctor, {0_v}),
      dasm(OPCODE_INVOKE_VIRTUAL, a_foo, {0_v}),
      // *goto_mie, // branch to target2
      // B2
      // target1,
      dasm(OPCODE_NEW_INSTANCE, type_b),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_INVOKE_DIRECT, b_ctor, {0_v}),
      dasm(OPCODE_INVOKE_VIRTUAL, b_foo, {0_v}),
      // target2,
      // B3
      // Coming out of one branch, v0 is a reference and coming out of the
      // other,
      // it's an integer.
      dasm(OPCODE_INVOKE_VIRTUAL, base_foo, {0_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };

  IRCode* code = m_method->get_code();
  code->push_back(*if_mie);
  code->push_back(insns[0]);
  code->push_back(insns[1]);
  code->push_back(insns[2]);
  code->push_back(insns[3]);
  code->push_back(*goto_mie);
  code->push_back(target1);
  code->push_back(insns[4]);
  code->push_back(insns[5]);
  code->push_back(insns[6]);
  code->push_back(insns[7]);
  code->push_back(target2);
  code->push_back(insns[8]);
  code->push_back(insns[9]);

  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  // Checks
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ("OK", checker.what());
  EXPECT_EQ(type_a, *checker.get_dex_type(insns[2], 0));
  EXPECT_EQ(type_a, *checker.get_dex_type(insns[3], 0));
  EXPECT_EQ(type_b, *checker.get_dex_type(insns[6], 0));
  EXPECT_EQ(type_b, *checker.get_dex_type(insns[7], 0));
  EXPECT_EQ(type_base, *checker.get_dex_type(insns[8], 0));
  EXPECT_EQ(type_base, *checker.get_dex_type(insns[9], 0));
}

/**
 * The bytecode stream of the following Java code.
 * A simple branch join scenario on a reference type.
 *
 * Base base = null;
 * if (condition) {
 *   base = new A();
 *   base.foo();
 * } else {
 *   base = new B();
 *   base.foo();
 * }
 * base.foo();
 */
TEST_F(IRTypeCheckerTest, joinCommonBaseWithConflictingInterface) {
  // Construct type hierarchy.
  const auto type_base = DexType::make_type("LBase;");
  const auto type_a = DexType::make_type("LA;");
  const auto type_b = DexType::make_type("LB;");
  const auto type_i = DexType::make_type("LI;");

  ClassCreator cls_base_creator(type_base);
  cls_base_creator.set_super(type::java_lang_Object());
  auto base_foo =
      DexMethod::make_method("LBase;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  cls_base_creator.add_method(base_foo);
  cls_base_creator.create();

  ClassCreator cls_a_creator(type_a);
  cls_a_creator.set_super(type_base);
  auto a_ctor = DexMethod::make_method("LA;.<init>:()V")
                    ->make_concrete(ACC_PUBLIC, false);
  cls_a_creator.add_method(a_ctor);
  auto a_foo =
      DexMethod::make_method("LA;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  cls_a_creator.add_method(a_foo);
  cls_a_creator.create();

  ClassCreator cls_b_creator(type_b);
  cls_b_creator.set_super(type_base);
  cls_b_creator.add_interface(type_i);

  auto b_ctor = DexMethod::make_method("LB;.<init>:()V")
                    ->make_concrete(ACC_PUBLIC, false);
  cls_b_creator.add_method(b_ctor);
  auto b_foo =
      DexMethod::make_method("LB;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  cls_b_creator.add_method(b_foo);
  cls_b_creator.create();

  // Construct code that references the above hierarchy.
  using namespace dex_asm;
  auto if_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {5_v}));
  auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO, {}));
  auto target1 = new BranchTarget(if_mie);
  auto target2 = new BranchTarget(goto_mie);

  std::vector<IRInstruction*> insns = {
      // B0
      // *if_mie, // branch to target1
      // B1
      dasm(OPCODE_NEW_INSTANCE, type_a),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_INVOKE_DIRECT, a_ctor, {0_v}),
      dasm(OPCODE_INVOKE_VIRTUAL, a_foo, {0_v}),
      // *goto_mie, // branch to target2
      // B2
      // target1,
      dasm(OPCODE_NEW_INSTANCE, type_b),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_INVOKE_DIRECT, b_ctor, {0_v}),
      dasm(OPCODE_INVOKE_VIRTUAL, b_foo, {0_v}),
      // target2,
      // B3
      // Coming out of one branch, v0 is a reference and coming out of the
      // other,
      // it's an integer.
      dasm(OPCODE_INVOKE_VIRTUAL, base_foo, {0_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };

  IRCode* code = m_method->get_code();
  code->push_back(*if_mie);
  code->push_back(insns[0]);
  code->push_back(insns[1]);
  code->push_back(insns[2]);
  code->push_back(insns[3]);
  code->push_back(*goto_mie);
  code->push_back(target1);
  code->push_back(insns[4]);
  code->push_back(insns[5]);
  code->push_back(insns[6]);
  code->push_back(insns[7]);
  code->push_back(target2);
  code->push_back(insns[8]);
  code->push_back(insns[9]);

  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  // Checks
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ("OK", checker.what());
  EXPECT_EQ(type_a, *checker.get_dex_type(insns[2], 0));
  EXPECT_EQ(type_a, *checker.get_dex_type(insns[3], 0));
  EXPECT_EQ(type_b, *checker.get_dex_type(insns[6], 0));
  EXPECT_EQ(type_b, *checker.get_dex_type(insns[7], 0));
  EXPECT_EQ(boost::none, checker.get_dex_type(insns[8], 0));
  EXPECT_EQ(boost::none, checker.get_dex_type(insns[9], 0));
}

/**
 * The bytecode stream of the following Java code.
 * A simple branch join scenario on a reference type.
 *
 * Base base = null;
 * if (condition) {
 *   base = new A();
 *   base.foo();
 * } else {
 *   base = new B();
 *   base.foo();
 * }
 * base.foo();
 */
TEST_F(IRTypeCheckerTest, joinCommonBaseWithMergableInterface) {
  // Construct type hierarchy.
  const auto type_base = DexType::make_type("LBase;");
  const auto type_a = DexType::make_type("LA;");
  const auto type_b = DexType::make_type("LB;");
  const auto type_i = DexType::make_type("LI;");

  ClassCreator cls_base_creator(type_base);
  cls_base_creator.set_super(type::java_lang_Object());
  cls_base_creator.add_interface(type_i);
  auto base_foo =
      DexMethod::make_method("LBase;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  cls_base_creator.add_method(base_foo);
  cls_base_creator.create();

  ClassCreator cls_a_creator(type_a);
  cls_a_creator.set_super(type_base);
  auto a_ctor = DexMethod::make_method("LA;.<init>:()V")
                    ->make_concrete(ACC_PUBLIC, false);
  cls_a_creator.add_method(a_ctor);
  auto a_foo =
      DexMethod::make_method("LA;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  cls_a_creator.add_method(a_foo);
  cls_a_creator.create();

  ClassCreator cls_b_creator(type_b);
  cls_b_creator.set_super(type_base);
  cls_b_creator.add_interface(type_i);

  auto b_ctor = DexMethod::make_method("LB;.<init>:()V")
                    ->make_concrete(ACC_PUBLIC, false);
  cls_b_creator.add_method(b_ctor);
  auto b_foo =
      DexMethod::make_method("LB;.foo:()I")->make_concrete(ACC_PUBLIC, true);
  cls_b_creator.add_method(b_foo);
  cls_b_creator.create();

  // Construct code that references the above hierarchy.
  using namespace dex_asm;
  auto if_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {5_v}));
  auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO, {}));
  auto target1 = new BranchTarget(if_mie);
  auto target2 = new BranchTarget(goto_mie);

  std::vector<IRInstruction*> insns = {
      // B0
      // *if_mie, // branch to target1
      // B1
      dasm(OPCODE_NEW_INSTANCE, type_a),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_INVOKE_DIRECT, a_ctor, {0_v}),
      dasm(OPCODE_INVOKE_VIRTUAL, a_foo, {0_v}),
      // *goto_mie, // branch to target2
      // B2
      // target1,
      dasm(OPCODE_NEW_INSTANCE, type_b),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_INVOKE_DIRECT, b_ctor, {0_v}),
      dasm(OPCODE_INVOKE_VIRTUAL, b_foo, {0_v}),
      // target2,
      // B3
      // Coming out of one branch, v0 is a reference and coming out of the
      // other,
      // it's an integer.
      dasm(OPCODE_INVOKE_VIRTUAL, base_foo, {0_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };

  IRCode* code = m_method->get_code();
  code->push_back(*if_mie);
  code->push_back(insns[0]);
  code->push_back(insns[1]);
  code->push_back(insns[2]);
  code->push_back(insns[3]);
  code->push_back(*goto_mie);
  code->push_back(target1);
  code->push_back(insns[4]);
  code->push_back(insns[5]);
  code->push_back(insns[6]);
  code->push_back(insns[7]);
  code->push_back(target2);
  code->push_back(insns[8]);
  code->push_back(insns[9]);

  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();
  // Checks
  EXPECT_TRUE(checker.good()) << checker.what();
  EXPECT_EQ("OK", checker.what());
  EXPECT_EQ(type_a, *checker.get_dex_type(insns[2], 0));
  EXPECT_EQ(type_a, *checker.get_dex_type(insns[3], 0));
  EXPECT_EQ(type_b, *checker.get_dex_type(insns[6], 0));
  EXPECT_EQ(type_b, *checker.get_dex_type(insns[7], 0));
  EXPECT_EQ(type_base, *checker.get_dex_type(insns[8], 0));
  EXPECT_EQ(type_base, *checker.get_dex_type(insns[9], 0));
}

/**
 * The bytecode stream of the following Java code.
 *
 * Base base;
 * if (condition) {
 *   base = null;
 * } else {
 *   base = new Object();
 * }
 * base.foo();
 */
TEST_F(IRTypeCheckerTest, invokeInvalidObjectType) {
  // Construct type hierarchy.
  const auto type_base = DexType::make_type("LBase;");

  ClassCreator cls_base_creator(type_base);
  cls_base_creator.set_super(type::java_lang_Object());
  auto base_foobar = DexMethod::make_method("LBase;.foobar:()I")
                         ->make_concrete(ACC_PUBLIC, true);
  cls_base_creator.add_method(base_foobar);
  cls_base_creator.create();

  auto object_ctor = DexMethod::make_method("Ljava/lang/Object;.<init>:()V");
  EXPECT_TRUE(object_ctor != nullptr);

  // Construct code that references the above hierarchy.
  using namespace dex_asm;
  auto if_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {5_v}));
  auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO, {}));
  auto target1 = new BranchTarget(if_mie);
  auto target2 = new BranchTarget(goto_mie);

  std::vector<IRInstruction*> insns = {
      // B0
      // *if_mie, // branch to target1
      // B1
      dasm(OPCODE_CONST, {0_v, 0_L}),
      // *goto_mie, // branch to target2
      // B2
      // target1,
      dasm(OPCODE_NEW_INSTANCE, type::java_lang_Object()),
      dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}),
      dasm(OPCODE_INVOKE_DIRECT, object_ctor, {0_v}),
      // target2,
      // B3
      // Coming out of one branch, v0 is null and coming out of the
      // other, it's an Object, but not (necessarily) a Base
      dasm(OPCODE_INVOKE_VIRTUAL, base_foobar, {0_v}),
      dasm(OPCODE_RETURN, {9_v}),
  };

  IRCode* code = m_method->get_code();
  code->push_back(*if_mie);
  code->push_back(insns[0]);
  code->push_back(*goto_mie);
  code->push_back(target1);
  code->push_back(insns[1]);
  code->push_back(insns[2]);
  code->push_back(insns[3]);
  code->push_back(target2);
  code->push_back(insns[4]);
  code->push_back(insns[5]);

  add_code(insns);
  IRTypeChecker checker(m_method);
  checker.run();

  // This should NOT type check successfully due to invoking Base.foobar against
  // an Object
  EXPECT_FALSE(checker.good()) << checker.what();
  EXPECT_NE("OK", checker.what());
}

TEST_F(IRTypeCheckerTest, invokeInitAfterNewInstance) {
  {
    auto method = DexMethod::make_method("LFoo;.bar:(LBar;)LFoo;")
                      ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (load-param-object v1)
        (new-instance "LFoo;")
        (move-result-pseudo-object v0)
        (invoke-direct (v0) "LFoo;.<init>:()V")
      )
    )"));
    IRTypeChecker checker(method);
    checker.run();
    EXPECT_TRUE(checker.good()) << checker.what();
  }
  {
    auto method = DexMethod::make_method("LFoo;.bar:(LBar;)LFoo;")
                      ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (load-param-object v1)
        (new-instance "LFoo;")
        (move-result-pseudo-object v0)
      )
    )"));
    IRTypeChecker checker(method);
    checker.run();
    EXPECT_TRUE(checker.good()) << checker.what();
  }

  {
    auto method = DexMethod::make_method("LFoo;.bar:(LBar;)LFoo;")
                      ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (load-param-object v1)
        (new-instance "LFoo;")
        (move-result-pseudo-object v0)
        (move-object v1 v0)
        (return-object v1)
      )
    )"));
    IRTypeChecker checker(method);
    checker.run();
    EXPECT_FALSE(checker.good());
    EXPECT_THAT(checker.what(),
                MatchesRegex("^Use of uninitialized variable.*"));
  }

  {
    auto method = DexMethod::make_method("LFoo;.bar:(LBar;)LFoo;")
                      ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    method->set_code(assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (load-param-object v1)
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (move-object v1 v0)
      (invoke-direct (v0) "LFoo;.<init>:()V")
      (return-object v1)
    )
  )"));
    IRTypeChecker checker(method);
    checker.run();
    EXPECT_TRUE(checker.good()) << checker.what();
  }

  {
    auto method = DexMethod::make_method("LFoo;.bar:(LBar;)LFoo;")
                      ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    method->set_code(assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (load-param-object v1)
      (new-instance "LFoo;")
      (move-result-pseudo-object v0)
      (move-object v1 v0)
      (return-object v1)
    )
  )"));
    IRTypeChecker checker(method);
    checker.run();
    EXPECT_FALSE(checker.good());
    EXPECT_THAT(checker.what(),
                MatchesRegex("^Use of uninitialized variable.*"));
  }

  {
    auto method = DexMethod::make_method("LFoo;.bar:(LBar;)LFoo;")
                      ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (load-param-object v1)
        (new-instance "LFoo;")
        (move-result-pseudo-object v0)
        (new-instance "LFoo;")
        (move-result-pseudo-object v5)
        (invoke-direct (v5) "LFoo;.<init>:()V")
        (return-object v5)
        (return-object v0)
      )
    )"));
    IRTypeChecker checker(method);
    checker.run();
    EXPECT_TRUE(checker.good()) << checker.what();
  }
}

TEST_F(IRTypeCheckerTest, checkNoOverwriteThis) {
  // Good
  {
    auto method = DexMethod::make_method("LFoo;.bar:(LBar;)LFoo;")
                      ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (load-param-object v1)
        (const v1 0)
        (return-object v0)
      )
    )"));
    IRTypeChecker checker(method);
    checker.run();
    EXPECT_TRUE(checker.good()) << checker.what();
  }
  // Bad: virtual method
  {
    auto method = DexMethod::make_method("LFoo;.bar:(LBar;)LFoo;")
                      ->make_concrete(ACC_PUBLIC, /* is_virtual */ true);
    method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (load-param-object v1)
        (const v0 0) ; overwrites `this` register
        (return-object v0)
      )
    )"));
    IRTypeChecker checker(method);
    checker.check_no_overwrite_this();
    checker.run();
    EXPECT_FALSE(checker.good());
    EXPECT_EQ(checker.what(),
              "Encountered overwrite of `this` register by CONST v0, 0");
  }
  // Bad: non-static (private) direct method
  {
    auto method = DexMethod::make_method("LFoo;.bar:(LBar;)LFoo;")
                      ->make_concrete(ACC_PRIVATE, /* is_virtual */ false);
    method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (load-param-object v1)
        (const v0 0) ; overwrites `this` register
        (return-object v0)
      )
    )"));
    IRTypeChecker checker(method);
    checker.check_no_overwrite_this();
    checker.run();
    EXPECT_FALSE(checker.good());
    EXPECT_EQ(checker.what(),
              "Encountered overwrite of `this` register by CONST v0, 0");
  }
}

TEST_F(IRTypeCheckerTest, loadParamVirtualFail) {
  m_virtual_method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param v0)
        (const v1 0)
        (return-object v1)
      )
    )"));
  IRTypeChecker checker(m_virtual_method);
  checker.run();
  EXPECT_TRUE(checker.fail());
  EXPECT_THAT(checker.what(),
              MatchesRegex("^First parameter must be loaded with "
                           "load-param-object: IOPCODE_LOAD_PARAM v0$"));
}

TEST_F(IRTypeCheckerTest, loadParamVirtualSuccess) {
  m_virtual_method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (load-param v1)
        (load-param v2)
        (load-param-wide v3)
        (load-param v4)
        (load-param-wide v5)
        (load-param v6)
        (load-param v7)
        (load-param-object v8)
        (return-object v8)
      )
    )"));
  IRTypeChecker checker(m_virtual_method);
  checker.run();
  EXPECT_FALSE(checker.fail());
}

TEST_F(IRTypeCheckerTest, loadParamStaticCountSuccess) {
  m_method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param v0)
        (load-param v1)
        (load-param-wide v2)
        (load-param v3)
        (load-param-wide v4)
        (load-param v5)
        (load-param v6)
        (load-param-object v7)
        (const v7 0)
        (return-object v7)
      )
    )"));
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_FALSE(checker.fail());
}

TEST_F(IRTypeCheckerTest, loadParamStaticCountLessFail) {
  m_method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param v0)
        (load-param v1)
        (load-param-wide v2)
        (const v3 0)
        (return-object v3)
      )
    )"));
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_EQ(checker.what(),
            "Number of existing load-param instructions (3) is lower than "
            "expected (8)");
}

TEST_F(IRTypeCheckerTest, loadParamInstanceCountLessFail) {
  m_virtual_method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (const v3 0)
        (return-object v3)
      )
    )"));
  IRTypeChecker checker(m_virtual_method);
  checker.run();
  EXPECT_EQ(checker.what(),
            "Number of existing load-param instructions (1) is lower than "
            "expected (9)");
}

TEST_F(IRTypeCheckerTest, loadParamInstanceCountMoreFail) {
  m_virtual_method->set_code(assembler::ircode_from_string(R"(
      (
        (load-param-object v0)
        (load-param v1)
        (load-param v2)
        (load-param-wide v3)
        (load-param v4)
        (load-param-wide v5)
        (load-param v6)
        (load-param v7)
        (load-param-object v8)
        (load-param v9)
        (const v7 0)
        (return-object v7)
      )
    )"));
  IRTypeChecker checker(m_virtual_method);
  checker.run();
  EXPECT_EQ(checker.what(),
            "Not enough argument types for IOPCODE_LOAD_PARAM v9");
}

TEST_F(IRTypeCheckerTest, loadParamStaticCountMoreFail) {
  m_method->set_code(assembler::ircode_from_string(R"(
    (
      (load-param v0)
      (load-param v1)
      (load-param-wide v2)
      (load-param v3)
      (load-param-wide v4)
      (load-param v5)
      (load-param v6)
      (load-param-object v7)
      (load-param v8)
      (return-object v7)
    )
    )"));
  IRTypeChecker checker(m_method);
  checker.run();
  EXPECT_EQ(checker.what(),
            "Not enough argument types for IOPCODE_LOAD_PARAM v8");
}

template <bool kVirtual>
class LoadParamMutationTest : public IRTypeCheckerTest {
 public:
  void run() {
    IRCode* code = (kVirtual ? m_virtual_method : m_method)->get_code();
    recurse(code, code->begin());
  }

  void recurse(IRCode* code, IRList::iterator it) {
    if (it == code->end()) {
      return;
    }

    if (it->type == MFLOW_OPCODE &&
        opcode::is_a_load_param(it->insn->opcode())) {
      auto real_op = it->insn->opcode();
      for (auto op = IOPCODE_LOAD_PARAM; op <= IOPCODE_LOAD_PARAM_WIDE;
           op = static_cast<IROpcode>(static_cast<uint16_t>(op) + 1)) {
        if (op == real_op) {
          continue;
        }
        it->insn->set_opcode(op);
        check_fail();
      }
      it->insn->set_opcode(real_op);
    }

    ++it;
    recurse(code, it);
  }

  void check_fail() {
    IRTypeChecker checker(kVirtual ? m_virtual_method : m_method);
    checker.run();
    EXPECT_TRUE(checker.fail());
  }
};

using LoadParamMutationStaticTest = LoadParamMutationTest<false>;
using LoadParamMutationVirtualTest = LoadParamMutationTest<true>;

TEST_F(LoadParamMutationStaticTest, mutate) { run(); }
TEST_F(LoadParamMutationVirtualTest, mutate) { run(); }
