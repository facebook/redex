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

std::ostream& operator<<(std::ostream& os, const IRInstruction& to_show) {
  return os << show(&to_show);
}

std::ostream& operator<<(std::ostream& os, const DexOpcode& to_show) {
  return os << show(to_show);
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

  for (DexOpcode op : all_opcodes) {
    // XXX we currently aren't testing these opcodes because they change when
    // we do the round-trip conversion. For example, `const v0` gets converted
    // to `const/4 v0`. To prevent these changes from happening, we can set the
    // operands to the largest values that can be encoded for a given opcode.
    if (is_move(op) ||
        (op >= OPCODE_CONST_4 && op <= OPCODE_CONST_WIDE_HIGH16)) {
      continue;
    }
    // XXX we can test these opcodes if we create a corresponding data payload
    if (op == OPCODE_FILL_ARRAY_DATA) {
      continue;
    }
    // XXX we eliminate NOPs, so no point testing them. As for opcodes with
    // offsets, they are tricky to test as sync() can change them depending on
    // the size of the offset.
    if (op == OPCODE_NOP || opcode::has_offset(op)) {
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
    if (opcode::has_literal(op)) {
      insn->set_literal(0xface);
    }
    if (opcode::has_range(op)) {
      insn->set_range_base(0xf);
      insn->set_range_size(0xf);
    }
    if (insn->has_arg_word_count()) {
      insn->set_arg_word_count(5);
    }
    if (insn->has_string()) {
      static_cast<DexOpcodeString*>(insn)->set_string(str);
    } else if (insn->has_type()) {
      static_cast<DexOpcodeType*>(insn)->set_type(ty);
    } else if (insn->has_field()) {
      static_cast<DexOpcodeField*>(insn)->set_field(field);
    } else if (insn->has_method()) {
      static_cast<DexOpcodeMethod*>(insn)->set_method(method);
    }

    method->set_dex_code(std::make_unique<DexCode>());
    method->get_dex_code()->get_instructions().push_back(insn);
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
  auto insn = dasm(OPCODE_INVOKE_VIRTUAL_RANGE, method);
  insn->set_range_base(1);
  insn->set_range_size(6);
  auto orig = new IRInstruction(*insn);

  insn->range_to_srcs();
  EXPECT_EQ(
      *insn,
      *dasm(OPCODE_INVOKE_VIRTUAL, method, {1_v, 2_v, 3_v, 4_v, 5_v, 6_v}));
  EXPECT_TRUE(needs_range_conversion(insn));

  insn->normalize_registers();
  EXPECT_EQ(*insn, *dasm(OPCODE_INVOKE_VIRTUAL, method, {1_v, 2_v, 4_v, 5_v}));

  insn->denormalize_registers();
  EXPECT_EQ(
      *insn,
      *dasm(OPCODE_INVOKE_VIRTUAL, method, {1_v, 2_v, 3_v, 4_v, 5_v, 6_v}));

  insn->srcs_to_range();
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
  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "V", {}));
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

  auto* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "V", {}));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);

  auto do_test = [&](IRInstruction* insn, DexInstruction* expected) {
    method->set_code(std::make_unique<IRCode>(method, 0));
    method->get_code()->push_back(insn);
    instruction_lowering::lower(method);
    EXPECT_EQ(*method->get_code()->begin()->dex_insn, *expected);
  };

  // Check that we recognize IRInstructions that can be converted to 2addr form
  do_test(
      dasm(OPCODE_ADD_INT, {0_v, 0_v, 1_v}),
      (new DexInstruction(OPCODE_ADD_INT_2ADDR))->set_src(0, 0)->set_src(1, 1));

  // IRInstructions that have registers beyond 4 bits can't benefit, however
  do_test(dasm(OPCODE_ADD_INT, {17_v, 17_v, 1_v}),
          (new DexInstruction(OPCODE_ADD_INT))
              ->set_dest(17)
              ->set_src(0, 17)
              ->set_src(1, 1));

  do_test(dasm(OPCODE_ADD_INT, {0_v, 0_v, 17_v}),
          (new DexInstruction(OPCODE_ADD_INT))
              ->set_dest(0)
              ->set_src(0, 0)
              ->set_src(1, 17));

  // Check that we take advantage of commutativity
  do_test(dasm(OPCODE_ADD_INT, {1_v, 0_v, 1_v}),
          (new DexInstruction(OPCODE_ADD_INT_2ADDR))
              ->set_src(0, 1)
              ->set_src(1, 0));

  // Check that we don't abuse commutativity if the operators aren't
  // commutative
  do_test(dasm(OPCODE_SUB_INT, {1_v, 0_v, 1_v}),
          (new DexInstruction(OPCODE_SUB_INT))
              ->set_dest(1)
              ->set_src(0, 0)
              ->set_src(1, 1));

  // check registers beyond 4 bits can't benefit
  do_test(dasm(OPCODE_ADD_INT, {17_v, 1_v, 17_v}),
          (new DexInstruction(OPCODE_ADD_INT))
              ->set_dest(17)
              ->set_src(0, 1)
              ->set_src(1, 17));

  delete g_redex;
}

TEST(IRInstruction, SelectCheckCast) {
  using namespace dex_asm;
  g_redex = new RedexContext();

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "V", {}));
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
      *(new DexInstruction(OPCODE_MOVE_OBJECT))->set_dest(0)->set_src(0, 1));
  ++it;
  EXPECT_EQ(*it->dex_insn,
            *(new DexOpcodeType(OPCODE_CHECK_CAST, get_object_type()))
                 ->set_src(0, 0));

  delete g_redex;
}

TEST(IRInstruction, SelectMove) {
  using namespace dex_asm;
  using namespace instruction_lowering::impl;
  g_redex = new RedexContext();

  EXPECT_EQ(OPCODE_MOVE, select_move_opcode(dasm(OPCODE_MOVE_16, {0_v, 0_v})));
  EXPECT_EQ(OPCODE_MOVE_FROM16,
            select_move_opcode(dasm(OPCODE_MOVE_16, {255_v, 65535_v})));
  EXPECT_EQ(OPCODE_MOVE_16,
            select_move_opcode(dasm(OPCODE_MOVE_16, {65535_v, 65535_v})));
  EXPECT_EQ(OPCODE_MOVE_OBJECT,
            select_move_opcode(dasm(OPCODE_MOVE_OBJECT_16, {0_v, 0_v})));
  EXPECT_EQ(OPCODE_MOVE_OBJECT_FROM16,
            select_move_opcode(dasm(OPCODE_MOVE_OBJECT_16, {255_v, 65535_v})));
  EXPECT_EQ(
      OPCODE_MOVE_OBJECT_16,
      select_move_opcode(dasm(OPCODE_MOVE_OBJECT_16, {65535_v, 65535_v})));

  delete g_redex;
}

TEST(IRInstruction, SelectConst) {
  using namespace dex_asm;
  using namespace instruction_lowering::impl;
  g_redex = new RedexContext();

  auto insn = dasm(OPCODE_CONST, {0_v});

  EXPECT_EQ(OPCODE_CONST_4, select_const_opcode(insn));

  insn->set_literal(std::numeric_limits<int16_t>::max());
  EXPECT_EQ(OPCODE_CONST_16, select_const_opcode(insn));
  insn->set_literal(std::numeric_limits<int16_t>::min());
  EXPECT_EQ(OPCODE_CONST_16, select_const_opcode(insn));

  insn->set_literal(static_cast<int32_t>(0xffff0000));
  EXPECT_EQ(OPCODE_CONST_HIGH16, select_const_opcode(insn));

  insn->set_literal(static_cast<int32_t>(0xffff0001));
  EXPECT_EQ(OPCODE_CONST, select_const_opcode(insn));

  auto wide_insn = dasm(OPCODE_CONST_WIDE, {0_v});

  EXPECT_EQ(OPCODE_CONST_WIDE_16, select_const_opcode(wide_insn));

  wide_insn->set_literal(static_cast<int32_t>(0xffff0001));
  EXPECT_EQ(OPCODE_CONST_WIDE_32, select_const_opcode(wide_insn));

  wide_insn->set_literal(0xffff000000000000);
  EXPECT_EQ(OPCODE_CONST_WIDE_HIGH16, select_const_opcode(wide_insn));

  wide_insn->set_literal(0xffff000000000001);
  EXPECT_EQ(OPCODE_CONST_WIDE, select_const_opcode(wide_insn));

  delete g_redex;
}
