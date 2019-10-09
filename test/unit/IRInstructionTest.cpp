/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "InstructionLowering.h"
#include "OpcodeList.h"
#include "RegAlloc.h"
#include "Show.h"

// for nicer gtest error messages
std::ostream& operator<<(std::ostream& os, const DexInstruction& to_show) {
  return os << show(&to_show);
}

std::ostream& operator<<(std::ostream& os, const IROpcode& to_show) {
  return os << show(to_show);
}

bool is_move(DexOpcode op) {
  return op >= DOPCODE_MOVE && op <= DOPCODE_MOVE_OBJECT_16;
}

TEST(IRInstruction, RoundTrip) {
  using namespace instruction_lowering::impl;
  g_redex = new RedexContext();

  DexType* ty = DexType::make_type("Lfoo;");
  DexString* str = DexString::make_string("foo");
  DexFieldRef* field = DexField::make_field(ty, str, ty);
  auto* method = static_cast<DexMethod*>(DexMethod::make_method(
      ty, str, DexProto::make_proto(ty, DexTypeList::make_type_list({}))));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);

  for (auto op : all_dex_opcodes) {
    // XXX we currently aren't testing these opcodes because they change when
    // we do the round-trip conversion. For example, `const v0` gets converted
    // to `const/4 v0`. To prevent these changes from happening, we can set the
    // operands to the largest values that can be encoded for a given opcode.
    if (is_move(op) ||
        (op >= DOPCODE_CONST_4 && op <= DOPCODE_CONST_WIDE_HIGH16) ||
        op == DOPCODE_CONST_STRING_JUMBO) {
      continue;
    }
    // XXX we can test these opcodes if we create a corresponding data payload
    if (op == DOPCODE_FILL_ARRAY_DATA) {
      continue;
    }
    // XXX we eliminate NOPs, so no point testing them. As for opcodes with
    // offsets, they are tricky to test as sync() can change them depending on
    // the size of the offset.
    if (op == DOPCODE_NOP || dex_opcode::has_offset(op)) {
      continue;
    }

    auto insn = DexInstruction::make_instruction(op);
    // populate the instruction args with non-zero values so we can check
    // if we have copied everything correctly
    if (insn->dests_size()) {
      insn->set_dest(0xf);
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      insn->set_src(i, i + 1);
    }
    if (dex_opcode::has_literal(op)) {
      insn->set_literal(0xface);
    }
    if (dex_opcode::has_range(op)) {
      insn->set_range_base(0xf);
      insn->set_range_size(0xf);
    }
    if (dex_opcode::has_arg_word_count(op)) {
      insn->set_arg_word_count(5);
    }
    if (insn->has_string()) {
      static_cast<DexOpcodeString*>(insn)->set_string(str);
    } else if (insn->has_type()) {
      static_cast<DexOpcodeType*>(insn)->set_type(ty);
    } else if (insn->has_field()) {
      static_cast<DexOpcodeField*>(insn)->set_field(field);
    } else if (insn->has_method()) {
      // XXX We can / should test method-bearing instructions -- just need to
      // generate a method with a proto that matches the number of registers we
      // are passing in
      continue;
    }

    method->set_dex_code(std::make_unique<DexCode>());
    method->get_dex_code()->get_instructions().push_back(insn);
    method->get_dex_code()->set_registers_size(0xff);
    method->balloon();
    instruction_lowering::lower(method);
    method->sync();
    EXPECT_EQ(*method->get_dex_code()->get_instructions().at(0), *insn)
        << "at " << show(op);

    delete insn;
  }

  delete g_redex;
}

TEST(IRInstruction, NormalizeInvoke) {
  using namespace dex_asm;
  g_redex = new RedexContext();

  auto method = DexMethod::make_method("LFoo;", "x", "V", {"J", "I", "J"});
  auto insn =
      dasm(OPCODE_INVOKE_VIRTUAL, method, {1_v, 2_v, 3_v, 4_v, 5_v, 6_v});
  EXPECT_TRUE(needs_range_conversion(insn));

  auto orig = new IRInstruction(*insn);

  insn->normalize_registers();
  EXPECT_EQ(*insn, *dasm(OPCODE_INVOKE_VIRTUAL, method, {1_v, 2_v, 4_v, 5_v}));

  insn->denormalize_registers();
  EXPECT_EQ(
      *insn,
      *dasm(OPCODE_INVOKE_VIRTUAL, method, {1_v, 2_v, 3_v, 4_v, 5_v, 6_v}));

  EXPECT_EQ(*insn, *orig);

  delete g_redex;
}

/*
 * Helper function to run select and then extract the resulting instruction
 * from the instruction list. The only reason it's a list is that const-cast
 * IRInstructions can expand into two instructions due to select. Everything
 * else is a simple one-to-one instruction mapping, and that's the case that
 * this makes easy to test.
 */
IRInstruction* select_instruction(IRInstruction* insn) {
  DexMethod* method =
      static_cast<DexMethod*>(DexMethod::make_method("Lfoo;", "bar", "V", {}));
  method->make_concrete(ACC_STATIC, 0);
  method->set_code(std::make_unique<IRCode>(method, 0));
  auto code = method->get_code();
  code->push_back(insn);
  instruction_lowering::lower(method);
  return code->begin()->insn;
}

TEST(IRInstruction, TwoAddr) {
  using namespace dex_asm;
  g_redex = new RedexContext();

  auto* method =
      static_cast<DexMethod*>(DexMethod::make_method("Lfoo;", "bar", "V", {}));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);

  auto do_test = [&](IRInstruction* insn, DexInstruction* expected) {
    method->set_code(std::make_unique<IRCode>(method, 0));
    method->get_code()->push_back(insn);
    instruction_lowering::lower(method);
    EXPECT_EQ(*method->get_code()->begin()->dex_insn, *expected);
  };

  // Check that we recognize IRInstructions that can be converted to 2addr form
  do_test(dasm(OPCODE_ADD_INT, {0_v, 0_v, 1_v}),
          (new DexInstruction(DOPCODE_ADD_INT_2ADDR))
              ->set_src(0, 0)
              ->set_src(1, 1));

  // IRInstructions that have registers beyond 4 bits can't benefit, however
  do_test(dasm(OPCODE_ADD_INT, {17_v, 17_v, 1_v}),
          (new DexInstruction(DOPCODE_ADD_INT))
              ->set_dest(17)
              ->set_src(0, 17)
              ->set_src(1, 1));

  do_test(dasm(OPCODE_ADD_INT, {0_v, 0_v, 17_v}),
          (new DexInstruction(DOPCODE_ADD_INT))
              ->set_dest(0)
              ->set_src(0, 0)
              ->set_src(1, 17));

  // Check that we take advantage of commutativity
  do_test(dasm(OPCODE_ADD_INT, {1_v, 0_v, 1_v}),
          (new DexInstruction(DOPCODE_ADD_INT_2ADDR))
              ->set_src(0, 1)
              ->set_src(1, 0));

  // Check that we don't abuse commutativity if the operators aren't
  // commutative
  do_test(dasm(OPCODE_SUB_INT, {1_v, 0_v, 1_v}),
          (new DexInstruction(DOPCODE_SUB_INT))
              ->set_dest(1)
              ->set_src(0, 0)
              ->set_src(1, 1));

  // check registers beyond 4 bits can't benefit
  do_test(dasm(OPCODE_ADD_INT, {17_v, 1_v, 17_v}),
          (new DexInstruction(DOPCODE_ADD_INT))
              ->set_dest(17)
              ->set_src(0, 1)
              ->set_src(1, 17));

  delete g_redex;
}

TEST(IRInstruction, SelectCheckCast) {
  using namespace dex_asm;
  g_redex = new RedexContext();

  DexMethod* method =
      static_cast<DexMethod*>(DexMethod::make_method("Lfoo;", "bar", "V", {}));
  method->make_concrete(ACC_STATIC, 0);
  method->set_code(std::make_unique<IRCode>(method, 0));
  auto code = method->get_code();
  code->push_back(dasm(OPCODE_CHECK_CAST, get_object_type(), {1_v}));
  code->push_back(dasm(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT, {0_v}));
  instruction_lowering::lower(method);

  // check that we inserted a move opcode before the check-cast
  auto it = code->begin();
  EXPECT_EQ(
      *it->dex_insn,
      *(new DexInstruction(DOPCODE_MOVE_OBJECT))->set_dest(0)->set_src(0, 1));
  ++it;
  EXPECT_EQ(*it->dex_insn,
            *(new DexOpcodeType(DOPCODE_CHECK_CAST, get_object_type()))
                 ->set_src(0, 0));

  delete g_redex;
}

TEST(IRInstruction, SelectMove) {
  using namespace dex_asm;
  using namespace instruction_lowering::impl;
  g_redex = new RedexContext();

  EXPECT_EQ(DOPCODE_MOVE, select_move_opcode(dasm(OPCODE_MOVE, {0_v, 0_v})));
  EXPECT_EQ(DOPCODE_MOVE_FROM16,
            select_move_opcode(dasm(OPCODE_MOVE, {255_v, 65535_v})));
  EXPECT_EQ(DOPCODE_MOVE_16,
            select_move_opcode(dasm(OPCODE_MOVE, {65535_v, 65535_v})));
  EXPECT_EQ(DOPCODE_MOVE_OBJECT,
            select_move_opcode(dasm(OPCODE_MOVE_OBJECT, {0_v, 0_v})));
  EXPECT_EQ(DOPCODE_MOVE_OBJECT_FROM16,
            select_move_opcode(dasm(OPCODE_MOVE_OBJECT, {255_v, 65535_v})));
  EXPECT_EQ(DOPCODE_MOVE_OBJECT_16,
            select_move_opcode(dasm(OPCODE_MOVE_OBJECT, {65535_v, 65535_v})));

  delete g_redex;
}

TEST(IRInstruction, SelectConst) {
  using namespace dex_asm;
  using namespace instruction_lowering::impl;
  g_redex = new RedexContext();

  auto insn = dasm(OPCODE_CONST, {0_v});
  EXPECT_EQ(DOPCODE_CONST_4, select_const_opcode(insn));

  insn->set_literal(0xf);
  // This has to be const/16 and not const/4 because sign extension will cause
  // const/4 0xf to load the value 0xffffffff into the dest register
  EXPECT_EQ(DOPCODE_CONST_16, select_const_opcode(insn));

  insn->set_literal(0xffffffffffffffff);
  // Conversely, this can use const/4 because of sign extension
  EXPECT_EQ(DOPCODE_CONST_4, select_const_opcode(insn));

  insn->set_literal(std::numeric_limits<int16_t>::max());
  EXPECT_EQ(DOPCODE_CONST_16, select_const_opcode(insn));
  insn->set_literal(std::numeric_limits<int16_t>::min());
  EXPECT_EQ(DOPCODE_CONST_16, select_const_opcode(insn));

  insn->set_literal(static_cast<int32_t>(0xffff0000));
  EXPECT_EQ(DOPCODE_CONST_HIGH16, select_const_opcode(insn));

  insn->set_literal(static_cast<int32_t>(0xffff0001));
  EXPECT_EQ(DOPCODE_CONST, select_const_opcode(insn));

  insn->set_literal(0xf0ffffffffffffff);
  EXPECT_THROW(select_const_opcode(insn), RedexException);

  auto wide_insn = dasm(OPCODE_CONST_WIDE, {0_v});

  EXPECT_EQ(DOPCODE_CONST_WIDE_16, select_const_opcode(wide_insn));

  wide_insn->set_literal(static_cast<int32_t>(0xffff0001));
  EXPECT_EQ(DOPCODE_CONST_WIDE_32, select_const_opcode(wide_insn));

  wide_insn->set_literal(0xffff000000000000);
  EXPECT_EQ(DOPCODE_CONST_WIDE_HIGH16, select_const_opcode(wide_insn));

  wide_insn->set_literal(0xffff000000000001);
  EXPECT_EQ(DOPCODE_CONST_WIDE, select_const_opcode(wide_insn));

  delete g_redex;
}

TEST(IRInstruction, SelectBinopLit) {
  using namespace dex_asm;
  using namespace instruction_lowering::impl;
  g_redex = new RedexContext();

  const IROpcode ops[] = {
      OPCODE_ADD_INT_LIT16, OPCODE_RSUB_INT,      OPCODE_MUL_INT_LIT16,
      OPCODE_DIV_INT_LIT16, OPCODE_REM_INT_LIT16, OPCODE_AND_INT_LIT16,
      OPCODE_OR_INT_LIT16,  OPCODE_XOR_INT_LIT16, OPCODE_ADD_INT_LIT8,
      OPCODE_RSUB_INT_LIT8, OPCODE_MUL_INT_LIT8,  OPCODE_DIV_INT_LIT8,
      OPCODE_REM_INT_LIT8,  OPCODE_AND_INT_LIT8,  OPCODE_OR_INT_LIT8,
      OPCODE_XOR_INT_LIT8,  OPCODE_SHL_INT_LIT8,  OPCODE_SHR_INT_LIT8,
      OPCODE_USHR_INT_LIT8};

  const DexOpcode expected_fit8[] = {
      DOPCODE_ADD_INT_LIT8,  DOPCODE_RSUB_INT_LIT8, DOPCODE_MUL_INT_LIT8,
      DOPCODE_DIV_INT_LIT8,  DOPCODE_REM_INT_LIT8,  DOPCODE_AND_INT_LIT8,
      DOPCODE_OR_INT_LIT8,   DOPCODE_XOR_INT_LIT8,  DOPCODE_ADD_INT_LIT8,
      DOPCODE_RSUB_INT_LIT8, DOPCODE_MUL_INT_LIT8,  DOPCODE_DIV_INT_LIT8,
      DOPCODE_REM_INT_LIT8,  DOPCODE_AND_INT_LIT8,  DOPCODE_OR_INT_LIT8,
      DOPCODE_XOR_INT_LIT8,  DOPCODE_SHL_INT_LIT8,  DOPCODE_SHR_INT_LIT8,
      DOPCODE_USHR_INT_LIT8};

  const DexOpcode expected_fit16[] = {
      DOPCODE_ADD_INT_LIT16, DOPCODE_RSUB_INT,      DOPCODE_MUL_INT_LIT16,
      DOPCODE_DIV_INT_LIT16, DOPCODE_REM_INT_LIT16, DOPCODE_AND_INT_LIT16,
      DOPCODE_OR_INT_LIT16,  DOPCODE_XOR_INT_LIT16, DOPCODE_ADD_INT_LIT16,
      DOPCODE_RSUB_INT,      DOPCODE_MUL_INT_LIT16, DOPCODE_DIV_INT_LIT16,
      DOPCODE_REM_INT_LIT16, DOPCODE_AND_INT_LIT16, DOPCODE_OR_INT_LIT16,
      DOPCODE_XOR_INT_LIT16};

  size_t count_inst = sizeof(ops) / sizeof(ops[0]);
  for (int i = 0; i < count_inst; i++) {
    // literal set to default size (0)
    auto insn = new IRInstruction(ops[i]);
    EXPECT_EQ(expected_fit8[i], select_binop_lit_opcode(insn))
        << "at " << show(ops[i]);

    // literal within 8 bits
    insn->set_literal(0x7f);
    EXPECT_EQ(expected_fit8[i], select_binop_lit_opcode(insn))
        << "at " << show(ops[i]);

    // literal within 16 bits
    insn->set_literal(0x7fff);
    if (ops[i] != OPCODE_SHL_INT_LIT8 && ops[i] != OPCODE_SHR_INT_LIT8 &&
        ops[i] != OPCODE_USHR_INT_LIT8) {
      EXPECT_EQ(expected_fit16[i], select_binop_lit_opcode(insn))
          << "at " << show(ops[i]);
    }

    // literal > 16 bits
    insn->set_literal(0xffffff);
    EXPECT_THROW(select_binop_lit_opcode(insn), RedexException)
        << "at " << show(ops[i]);
  }
  delete g_redex;
}

TEST(IRInstruction, InvokeSourceIsWideBasic) {
  using namespace dex_asm;
  g_redex = new RedexContext();

  DexMethodRef* m = DexMethod::make_method("Lfoo;", "baz", "V", {"J"});
  IRInstruction* insn = new IRInstruction(OPCODE_INVOKE_STATIC);
  insn->set_arg_word_count(1);
  insn->set_src(0, 0);
  insn->set_method(m);

  EXPECT_TRUE(insn->invoke_src_is_wide(0));

  delete g_redex;
}

TEST(IRInstruction, InvokeSourceIsWideComplex) {
  g_redex = new RedexContext();

  IRInstruction* insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
  DexMethodRef* m =
      DexMethod::make_method("Lfoo;", "qux", "V", {"I", "J", "I"});
  insn->set_method(m);
  insn->set_arg_word_count(4);
  insn->set_src(0, 1);
  insn->set_src(1, 0);
  insn->set_src(2, 2);
  insn->set_src(3, 3);

  EXPECT_FALSE(insn->invoke_src_is_wide(0));
  EXPECT_FALSE(insn->invoke_src_is_wide(1));
  EXPECT_TRUE(insn->invoke_src_is_wide(2));
  EXPECT_FALSE(insn->invoke_src_is_wide(3));

  delete g_redex;
}

TEST(IRInstruction, InvokeSourceIsWideComplex2) {
  g_redex = new RedexContext();

  IRInstruction* insn = new IRInstruction(OPCODE_INVOKE_VIRTUAL);
  DexMethodRef* m =
      DexMethod::make_method("Lfoo;", "qux", "V", {"I", "J", "I", "J"});
  insn->set_method(m);
  insn->set_arg_word_count(5);
  insn->set_src(0, 0);
  insn->set_src(1, 1);
  insn->set_src(2, 2);
  insn->set_src(3, 3);
  insn->set_src(4, 4);

  EXPECT_FALSE(insn->invoke_src_is_wide(0));
  EXPECT_FALSE(insn->invoke_src_is_wide(1));
  EXPECT_TRUE(insn->invoke_src_is_wide(2));
  EXPECT_FALSE(insn->invoke_src_is_wide(3));
  EXPECT_TRUE(insn->invoke_src_is_wide(4));

  delete g_redex;
}
