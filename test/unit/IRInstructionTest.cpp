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
#include "InstructionSelection.h"
#include "OpcodeList.h"
#include "RegAlloc.h"
#include "Show.h"

using namespace select_instructions;

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
  g_redex = new RedexContext();

  DexType* ty = DexType::make_type("Lfoo;");
  DexString* str = DexString::make_string("foo");
  DexField* field = DexField::make_field(ty, str, ty);
  DexMethod* method = DexMethod::make_method(
      ty, str, DexProto::make_proto(ty, DexTypeList::make_type_list({})));

  for (DexOpcode op : all_opcodes) {
    auto insn = DexInstruction::make_instruction(op);
    // populate the instruction args with non-zero values so we can check
    // if we have copied everything correctly
    if (insn->dests_size()) {
      insn->set_dest(0xf);
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      insn->set_src(i, i + 1);
    }
    if (opcode::has_offset(op)) {
      insn->set_offset(0xf);
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

    auto ir_insn = new IRInstruction(insn);
    try_2addr_conversion(ir_insn);
    EXPECT_EQ(*ir_insn->to_dex_instruction(), *insn) << "at " << show(op);

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
  DexMethod* method = DexMethod::make_method("Lfoo;", "bar", "V", {});
  method->make_concrete(ACC_STATIC, 0);
  auto code = std::make_unique<IRCode>(method, 0);
  code->push_back(insn);
  InstructionSelection select;
  select.select_instructions(code.get());
  return code->begin()->insn;
}

TEST(IRInstruction, TwoAddr) {
  using namespace dex_asm;
  g_redex = new RedexContext();

  // check that we recognize IRInstructions that can be converted to 2addr form
  EXPECT_EQ(*select_instruction(dasm(OPCODE_ADD_INT, {0_v, 0_v, 1_v})),
            *dasm(OPCODE_ADD_INT_2ADDR, {0_v, 1_v}));
  // IRInstructions that have registers beyond 4 bits can't benefit, however
  EXPECT_EQ(*select_instruction(dasm(OPCODE_ADD_INT, {17_v, 17_v, 1_v})),
            *dasm(OPCODE_ADD_INT, {17_v, 17_v, 1_v}));
  EXPECT_EQ(*select_instruction(dasm(OPCODE_ADD_INT, {0_v, 0_v, 17_v})),
            *dasm(OPCODE_ADD_INT, {0_v, 0_v, 17_v}));
  // check converting to 2addr form work for add-int v1,v0,v1
  EXPECT_EQ(*select_instruction(dasm(OPCODE_ADD_INT, {1_v, 0_v, 1_v})),
            *dasm(OPCODE_ADD_INT_2ADDR, {1_v, 0_v}));
  // check only commutative instruction can be converted to 2addr
  // for form OPCODE v1,v0,v1
  EXPECT_EQ(*select_instruction(dasm(OPCODE_SUB_INT, {1_v, 0_v, 1_v})),
            *dasm(OPCODE_SUB_INT, {1_v, 0_v, 1_v}));
  // check registers beyond 4 bits can't benefit
  EXPECT_EQ(*select_instruction(dasm(OPCODE_ADD_INT, {17_v, 1_v, 17_v})),
            *dasm(OPCODE_ADD_INT, {17_v, 1_v, 17_v}));
  delete g_redex;
}

TEST(IRInstruction, SelectCheckCast) {
  using namespace dex_asm;
  g_redex = new RedexContext();

  DexMethod* method = DexMethod::make_method("Lfoo;", "bar", "V", {});
  method->make_concrete(ACC_STATIC, 0);
  auto code = std::make_unique<IRCode>(method, 0);
  code->push_back(dasm(OPCODE_CHECK_CAST, get_object_type(), {0_v, 1_v}));
  InstructionSelection select;
  select.select_instructions(code.get());

  // check that we inserted a move opcode before the check-cast
  auto it = InstructionIterable(code.get()).begin();
  EXPECT_EQ(*it->insn, *dasm(OPCODE_MOVE_OBJECT, {0_v, 1_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CHECK_CAST, get_object_type(), {0_v, 0_v}));

  delete g_redex;
}

TEST(IRInstruction, SelectMove) {
  using namespace dex_asm;
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
