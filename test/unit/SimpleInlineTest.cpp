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
#include "Inliner.h"
#include "IRCode.h"
#include "RedexTest.h"

struct SimpleInlineTest : public RedexTest {};

std::ostream& operator<<(std::ostream& os, const IRInstruction& to_show) {
  return os << show(&to_show);
}

/*
 * Test that we correctly insert move instructions that map caller args to
 * callee params.
 */
TEST_F(SimpleInlineTest, insertMoves) {
  using namespace dex_asm;
  auto callee = static_cast<DexMethod*>(DexMethod::make_method(
      "Lfoo;", "testCallee", "V", {"I", "Ljava/lang/Object;"}));
  callee->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  callee->set_code(std::make_unique<IRCode>(callee, 0));

  auto caller = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "testCaller", "V", {}));
  caller->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  caller->set_code(std::make_unique<IRCode>(caller, 0));

  auto invoke = dasm(OPCODE_INVOKE_STATIC, callee, {});
  invoke->set_arg_word_count(2);
  invoke->set_src(0, 1);
  invoke->set_src(1, 2);

  auto caller_code = caller->get_code();
  caller_code->push_back(dasm(OPCODE_CONST, {1_v, 1_L}));
  caller_code->push_back(dasm(OPCODE_CONST, {2_v, 0_L})); // load null ref
  caller_code->push_back(invoke);
  auto invoke_it = std::prev(caller_code->end());
  caller_code->push_back(dasm(OPCODE_RETURN_VOID));
  caller_code->set_registers_size(3);

  auto callee_code = callee->get_code();
  callee_code->push_back(dasm(OPCODE_CONST, {1_v, 1_L}));
  callee_code->push_back(dasm(OPCODE_RETURN_VOID));

  inliner::inline_method(caller->get_code(), callee->get_code(), invoke_it);

  auto it = InstructionIterable(caller_code).begin();
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {1_v, 1_L}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {2_v, 0_L}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_MOVE, {3_v, 1_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_MOVE_OBJECT, {4_v, 2_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {4_v, 1_L}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_RETURN_VOID));

  EXPECT_EQ(caller_code->get_registers_size(), 5);
}
