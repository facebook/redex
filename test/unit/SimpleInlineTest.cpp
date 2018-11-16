/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "Inliner.h"
#include "RedexTest.h"

struct SimpleInlineTest : public RedexTest {};

void test_inliner(const std::string& caller_str,
                  const std::string& callee_str,
                  const std::string& expected_str) {
  auto caller = assembler::ircode_from_string(caller_str);
  auto callee = assembler::ircode_from_string(callee_str);

  const auto& callsite = std::find_if(
      caller->begin(), caller->end(), [](const MethodItemEntry& mie) {
        return mie.type == MFLOW_OPCODE && is_invoke(mie.insn->opcode());
      });
  inliner::inline_method(caller.get(), callee.get(), callsite);

  auto expected = assembler::ircode_from_string(expected_str);

  EXPECT_EQ(assembler::to_string(expected.get()),
            assembler::to_string(caller.get()));
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

TEST_F(SimpleInlineTest, debugPositionsAfterReturn) {
  DexMethod* caller =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.caller:()V"));
  caller->make_concrete(ACC_PUBLIC, /* is_virtual */ false);
  DexMethod* callee =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.callee:()V"));
  callee->make_concrete(ACC_PUBLIC, /* is_virtual */ false);
  const auto& caller_str = R"(
    (
      (.pos "LFoo;.caller:()V" "Foo.java" 10)
      (const v0 0)
      (invoke-static () "LFoo;.bar:()V")
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      (.pos "LFoo;.callee:()V" "Foo.java" 123)
      (const v0 1)
      (if-eqz v0 :after)

      (:exit)
      (.pos "LFoo;.callee:()V" "Foo.java" 124)
      (const v1 2)
      (return-void)

      (:after)
      (const v2 3)
      (goto :exit)
    )
  )";
  const auto& expected_str = R"(
    (
      (.pos "LFoo;.caller:()V" "Foo.java" 10)
      (const v0 0)

      (.pos "LFoo;.callee:()V" "Foo.java" 123 0)
      (const v1 1)
      (if-eqz v1 :after)

      (:exit)
      (.pos "LFoo;.callee:()V" "Foo.java" 124 0)
      (const v2 2)
      (.pos "LFoo;.caller:()V" "Foo.java" 10)
      (return-void)

      ; Check that this position was correctly added to the code after the
      ; callee's return
      (.pos "LFoo;.callee:()V" "Foo.java" 124 0)
      (:after)
      (const v3 3)
      (goto :exit)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}
