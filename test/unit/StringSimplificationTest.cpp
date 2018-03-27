/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexUnitTestRunner.h"
#include "DexUtil.h"

#include "StringSimplification.h"

using namespace dex_asm;

//========== Helpers ==========

IRInstruction* make_noise_instructions(uint16_t dest,
                                       uint16_t src_a,
                                       uint16_t src_b) {
  return dasm(OPCODE_ADD_INT, {{VREG, dest}, {VREG, src_a}, {VREG, src_b}});
}

IRInstruction* make_const_string(uint16_t dest, std::string s) {
  auto insn = new IRInstruction(OPCODE_CONST_STRING);
  insn->set_string(DexString::make_string(s));
  insn->set_dest(dest);
  return insn;
}

IRInstruction* make_stringbuilder(uint16_t dest) {
  auto insn = new IRInstruction(OPCODE_NEW_INSTANCE);
  insn->set_type(DexType::make_type("Ljava/lang/StringBuilder;"));
  insn->set_dest(dest);
  return insn;
}

IRInstruction* make_constructor(uint16_t dest) {
  auto insn = new IRInstruction(OPCODE_INVOKE_DIRECT);
  insn->set_arg_word_count(1);
  insn->set_src(0, dest);
  insn->set_method(
      DexMethod::make_method("Ljava/lang/StringBuilder;", "<init>", "V", {}));
  return insn;
}

IRInstruction* make_append_instruction(uint16_t vreg_sb, uint16_t vreg_str) {
  auto insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
  insn->set_arg_word_count(2);
  insn->set_src(0, vreg_sb);
  insn->set_src(1, vreg_str);
  insn->set_method(DexMethod::make_method("Ljava/lang/StringBuilder;",
                                          "append",
                                          "Ljava/lang/StringBuilder;",
                                          {"Ljava/lang/String;"}));
  return insn;
}

IRInstruction* make_to_string(uint16_t dest) {
  auto insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
  insn->set_arg_word_count(1);
  insn->set_src(0, dest);
  insn->set_method(DexMethod::make_method(
      "Ljava/lang/StringBuilder;", "toString", "Ljava/lang/String;", {}));
  return insn;
}

//========== Test Cases ==========

// Check that unicode strings can be appened together.
TEST(StringSimplification, testUnicodeStrings) {
  DexUnitTestRunner runner;

  auto parent = runner.create_class("Lcom/redex/UnicodeTest;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(1, "Привет!"));
  code->push_back(make_const_string(2, "TWO"));
  code->push_back(make_stringbuilder(3));
  code->push_back(make_constructor(3));

  code->push_back(make_append_instruction(3, 1));
  code->push_back(make_append_instruction(3, 2));
  code->push_back(make_to_string(3));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {3_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);
  runner.run(new StringSimplificationPass());
  std::vector<DexString*> arr;
  code->gather_strings(arr);
  EXPECT_TRUE(std::any_of(
      arr.begin(), arr.end(), [](auto x) { return "Привет!TWO" == x->str(); }));
}

// Check that the const string appears, and that no string builder instructions
// remain.
TEST(StringSimplification, testConstString) {
  DexUnitTestRunner runner;
  auto parent = runner.create_class("Lcom/redex/Parent2;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(1, "ONE "));
  code->push_back(make_const_string(6, "THREE"));
  code->push_back(make_const_string(18, "TWO "));

  code->push_back(make_stringbuilder(2));
  code->push_back(make_constructor(2));

  code->push_back(make_append_instruction(2, 1));
  code->push_back(make_append_instruction(2, 18));
  code->push_back(make_append_instruction(2, 6));
  code->push_back(make_to_string(2));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {2_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);
  runner.run(new StringSimplificationPass());
  std::vector<DexString*> arr;
  code->gather_strings(arr);

  EXPECT_TRUE(std::any_of(arr.begin(), arr.end(), [](auto x) {
    return "ONE TWO THREE" == x->str();
  }));
  for (auto& mie : InstructionIterable(code)) {
    EXPECT_NE(mie.insn->opcode(), OPCODE_INVOKE_VIRTUAL);
    EXPECT_NE(mie.insn->opcode(), OPCODE_INVOKE_DIRECT);
    EXPECT_NE(mie.insn->opcode(), OPCODE_NEW_INSTANCE);
  }
}

// Check that two constant string interrelations are converted while they are
// intertwined.
// StringBuilder v4 -> "THREE TWO ONE"
// StringBuilder v5 -> "TWO ONE THREE"
TEST(StringSimplification, testMultipleConstantBuilders) {
  DexUnitTestRunner runner;
  auto parent = runner.create_class("Lcom/redex/Parent3;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(11, "ONE"));
  code->push_back(make_const_string(13, "THREE"));
  code->push_back(make_const_string(12, "TWO"));

  code->push_back(make_stringbuilder(4));
  code->push_back(make_constructor(4));

  code->push_back(make_stringbuilder(5));
  code->push_back(make_constructor(5));

  code->push_back(make_append_instruction(4, 13));
  code->push_back(make_append_instruction(5, 12));

  code->push_back(make_append_instruction(4, 12));
  code->push_back(make_append_instruction(5, 11));

  code->push_back(make_append_instruction(4, 11));
  code->push_back(make_append_instruction(5, 13));

  code->push_back(make_to_string(4));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {2_v}));

  code->push_back(make_to_string(5));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {9_v}));

  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);
  runner.run(new StringSimplificationPass());
  std::vector<DexString*> arr;
  code->gather_strings(arr);

  EXPECT_TRUE(std::any_of(arr.begin(), arr.end(), [](auto x) {
    return "THREETWOONE" == x->str();
  }));
  EXPECT_TRUE(std::any_of(arr.begin(), arr.end(), [](auto x) {
    return "TWOONETHREE" == x->str();
  }));
  for (auto& mie : InstructionIterable(code)) {
    EXPECT_NE(mie.insn->opcode(), OPCODE_INVOKE_VIRTUAL);
    EXPECT_NE(mie.insn->opcode(), OPCODE_INVOKE_DIRECT);
    EXPECT_NE(mie.insn->opcode(), OPCODE_NEW_INSTANCE);
  }
}

// Before: A stringbuilder is appended to with additional noise inbetween.
// After: The stringbuilder is deleted, and the result is stored as a constant.
//        The number of noise instructions shouldn't be changed.
TEST(StringSimplification, testInterleavedInstructions) {
  DexUnitTestRunner runner;
  auto parent = runner.create_class("Lcom/redex/Parent4;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(11, "ONE"));

  code->push_back(dasm(OPCODE_CONST, {54_v, 1_L}));
  code->push_back(dasm(OPCODE_CONST, {55_v, 23_L}));
  code->push_back(make_noise_instructions(54, 54, 55));

  code->push_back(make_stringbuilder(4));
  code->push_back(make_constructor(4));

  code->push_back(make_const_string(13, "THREE"));
  code->push_back(make_const_string(12, "TWO"));

  code->push_back(make_noise_instructions(54, 54, 55));

  code->push_back(make_append_instruction(4, 13));
  code->push_back(make_noise_instructions(54, 54, 55));
  code->push_back(make_append_instruction(4, 12));
  code->push_back(make_noise_instructions(54, 54, 55));
  code->push_back(make_append_instruction(4, 11));

  code->push_back(make_to_string(4));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {4_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);
  runner.run(new StringSimplificationPass());
  std::vector<DexString*> arr;
  code->gather_strings(arr);

  EXPECT_EQ(4, std::count_if(code->begin(), code->end(), [](auto& x) {
              return x.type == MFLOW_OPCODE &&
                     x.insn->opcode() == OPCODE_ADD_INT;
            }));

  EXPECT_TRUE(std::any_of(arr.begin(), arr.end(), [](auto x) {
    return "THREETWOONE" == x->str();
  }));
  for (auto& mie : InstructionIterable(code)) {
    EXPECT_NE(mie.insn->opcode(), OPCODE_INVOKE_VIRTUAL);
    EXPECT_NE(mie.insn->opcode(), OPCODE_INVOKE_DIRECT);
    EXPECT_NE(mie.insn->opcode(), OPCODE_NEW_INSTANCE);
  }
}

// Before: 3 blocks, A -> B and A -> C.  Both diverge with string result.
// After: block B should have "THREEONE" and block C should have "THREETWO"
//        and the two blocks shouldn't have any stringbuilder code
TEST(StringSimplification, testBranching) {
  DexUnitTestRunner runner;
  auto parent = runner.create_class("Lcom/redex/Parent5;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(11, "ONE"));
  code->push_back(make_const_string(13, "THREE"));
  code->push_back(make_const_string(12, "TWO"));

  code->push_back(make_stringbuilder(4));
  code->push_back(make_constructor(4));

  code->push_back(make_append_instruction(4, 13));
  code->push_back(dasm(OPCODE_CONST, {6_v, 0_L}));

  auto insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
  insn->set_arg_word_count(1);
  insn->set_src(0, 6);
  insn->set_dest(6);
  insn->set_method(
      DexMethod::make_method("Ljava/lang/Funky;", "doTheThing", "B", {}));
  code->push_back(insn);

  auto if_mie = new MethodItemEntry(dasm(OPCODE_IF_EQZ, {6_v}));
  auto target = new BranchTarget(if_mie);
  code->push_back(*if_mie);
  code->push_back(make_append_instruction(4, 11));
  code->push_back(make_to_string(4));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {5_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->push_back(target);
  code->push_back(make_append_instruction(4, 12));
  code->push_back(make_to_string(4));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {5_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);
  code->build_cfg();
  runner.run(new StringSimplificationPass());

  std::vector<DexString*> arr;
  code->gather_strings(arr);
  EXPECT_TRUE(std::any_of(arr.begin(), arr.end(), [](auto x) {
    return "THREEONE" == x->str();
  })); // Check block 1.
  EXPECT_TRUE(std::any_of(arr.begin(), arr.end(), [](auto x) {
    return "THREETWO" == x->str();
  })); // Check block 2.
}

// Before: 2 blocks A -> B.  B's first instruction is toString.
// After: Replace toString with const-string "THREEONE".
// Test shouldn't crash during execution of runner. (beginning of block test)
TEST(StringSimplification, testBeginningOfBlockToString) {
  DexUnitTestRunner runner;
  auto parent = runner.create_class("Lcom/redex/Parent6;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(11, "ONE"));
  code->push_back(make_const_string(13, "THREE"));
  code->push_back(make_stringbuilder(4));
  code->push_back(make_constructor(4));

  code->push_back(make_append_instruction(4, 13));
  code->push_back(make_append_instruction(4, 11));
  auto goto_mie = new MethodItemEntry(dasm(OPCODE_GOTO));
  auto target = new BranchTarget(goto_mie);
  code->push_back(*goto_mie);
  code->push_back(make_noise_instructions(54, 54, 54));
  code->push_back(target);
  code->push_back(make_to_string(4));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {5_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);
  code->build_cfg();
  runner.run(new StringSimplificationPass());
  code->build_cfg();
  printf("Final Cfg: %s\n", SHOW(code->cfg()));
}

// If we pass a stringbuilder into a method, we shouldn't modify the code.
// Since the method can append at will, we must assume the builder becomes top.
TEST(StringSimplification, passStringBuilderInMethod) {
  DexUnitTestRunner runner;
  auto parent = runner.create_class("Lcom/redex/ParentBuilderInMethod;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(2, "TEST STRING TWO "));

  code->push_back(make_stringbuilder(3));
  code->push_back(make_constructor(3));
  code->push_back(make_append_instruction(3, 2));
  code->push_back(dasm(OPCODE_CONST, {6_v, 0_L}));

  auto insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
  insn->set_arg_word_count(2);
  insn->set_src(0, 6);
  insn->set_src(1, 3);
  insn->set_method(DexMethod::make_method(
      "Ljava/lang/Funky;", "doTheThing", "V", {"Ljava/lang/StringBuilder;"}));
  code->push_back(insn);
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {6_v}));

  code->push_back(make_to_string(3));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {3_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);
  runner.run(new StringSimplificationPass());
  EXPECT_EQ(10, code->count_opcodes());
  for (auto& mie : InstructionIterable(code)) {
    if ((mie.insn->opcode() == OPCODE_INVOKE_VIRTUAL ||
         mie.insn->opcode() == OPCODE_INVOKE_DIRECT) &&
        mie.insn->get_method()->get_class() ==
            DexType::make_type("Ljava/lang/Stringbuilder;")) {
      EXPECT_TRUE(false);
    }
  }
  code->build_cfg();
  printf("Final Cfg: %s\n", SHOW(code->cfg()));
}

// Check that interleaved stringbuilders do not mess with each other.
// StringBuilder v4 -> "foobar"
// StringBuilder v5 -> x + "bar"
TEST(StringSimplification, oneKnownOneUnkownBuilder) {
  DexUnitTestRunner runner;
  auto parent = runner.create_class("Lcom/redex/OneKnownOneUnkown;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(1, "foo"));
  code->push_back(make_const_string(2, "bar"));

  code->push_back(make_stringbuilder(4));
  code->push_back(make_constructor(4));

  code->push_back(make_stringbuilder(5));
  code->push_back(make_constructor(5));
  code->push_back(dasm(OPCODE_CONST, {6_v, 0_L}));

  code->push_back(make_append_instruction(4, 1));
  code->push_back(make_append_instruction(5, 6));

  code->push_back(make_append_instruction(4, 2));
  code->push_back(make_append_instruction(5, 2));

  code->push_back(make_to_string(4));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {2_v}));

  code->push_back(make_to_string(5));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {9_v}));

  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);
  runner.run(new StringSimplificationPass());
  code->build_cfg();
  printf("Final Cfg: %s\n", SHOW(code->cfg()));

  std::vector<DexString*> arr;
  code->gather_strings(arr);

  EXPECT_TRUE(std::any_of(
      arr.begin(), arr.end(), [](auto x) { return "foobar" == x->str(); }));
  auto count = 0;
  for (auto& mie : InstructionIterable(code)) {
    if (mie.insn->opcode() == OPCODE_INVOKE_VIRTUAL ||
        mie.insn->opcode() == OPCODE_INVOKE_DIRECT) {
      ASSERT_EQ(5, mie.insn->src(0));
      ++count;
    }
  }
  ASSERT_EQ(4, count);
}

// Before: sb = new StringBuilder()
//         x = someRandomString()    // "a"
//         sb.append(x).append("foo");
//         x = someOtherString()     // "b"
//         sb.toString()            // has value x + "", but wrong x.
// After:
//         Don't change
TEST(StringSimplification, modificationOfBaseVariable) {
  DexUnitTestRunner runner;

  auto parent = runner.create_class("Lcom/redex/ParentTestModification;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(1, "TEST STRING ONE "));
  code->push_back(make_stringbuilder(3));
  code->push_back(make_constructor(3));
  code->push_back(dasm(OPCODE_CONST, {6_v, 0_L}));

  auto insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
  insn->set_arg_word_count(1);
  insn->set_src(0, 6);
  insn->set_method(DexMethod::make_method(
      "Ljava/lang/Funky;", "doTheThing", "Ljava/lang/String;", {}));
  code->push_back(insn);
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {6_v}));

  code->push_back(make_append_instruction(3, 6));
  code->push_back(make_append_instruction(3, 1));

  insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
  insn->set_arg_word_count(1);
  insn->set_src(0, 6);
  insn->set_method(DexMethod::make_method(
      "Ljava/lang/Funky;", "doTheThing2", "Ljava/lang/String;", {}));
  code->push_back(insn);
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {6_v}));

  code->push_back(make_to_string(3));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {3_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);

  code->build_cfg();
  printf("Initial Cfg: %s\n", SHOW(code->cfg()));
  runner.run(new StringSimplificationPass());

  code->build_cfg();
  printf("Final Cfg: %s\n", SHOW(code->cfg()));
  EXPECT_EQ(13, code->count_opcodes());
}

// Check that the pointer aliasing is supported. (a stringbuilder
// is referenced via two registers but the state is shared correctly)
TEST(StringSimplification, registerAliasingTest) {
  DexUnitTestRunner runner;

  auto parent = runner.create_class("Lcom/redex/registerAliasingTest;");
  auto clinit = parent->get_clinit();
  auto code = clinit->get_code();

  code->push_back(make_const_string(1, "TEST"));

  code->push_back(make_stringbuilder(3));
  code->push_back(make_constructor(3));

  code->push_back(make_append_instruction(3, 1));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {2_v}));
  code->push_back(make_append_instruction(2, 1));
  code->push_back(make_append_instruction(2, 1));

  code->push_back(make_append_instruction(3, 1));

  code->push_back(make_to_string(3));
  code->push_back(dasm(OPCODE_MOVE_RESULT_OBJECT, {3_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  code->set_registers_size(9001);

  code->build_cfg();
  printf("Initial Cfg: %s\n", SHOW(code->cfg()));
  runner.run(new StringSimplificationPass());

  code->build_cfg();
  printf("Final Cfg: %s\n", SHOW(code->cfg()));

  std::vector<DexString*> arr;
  code->gather_strings(arr);

  EXPECT_TRUE(std::any_of(arr.begin(), arr.end(), [](auto x) {
    return "TESTTESTTESTTEST" == x->str();
  }));
  for (auto& mie : InstructionIterable(code)) {
    EXPECT_NE(mie.insn->opcode(), OPCODE_INVOKE_DIRECT);
    EXPECT_NE(mie.insn->opcode(), OPCODE_INVOKE_VIRTUAL);
  }
}
