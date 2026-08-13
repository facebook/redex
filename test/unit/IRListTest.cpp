/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "Debug.h"
#include "DexInstruction.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IRList.h"
#include "IROpcode.h"
#include "RedexTest.h"

class IRListTest : public RedexTest {};

TEST_F(IRListTest, method_item_entry_equality) {
  std::string s_insns = R"(
    (
      (load-param v0)
      (.dbg DBG_SET_PROLOGUE_END)

      (.try_start foo)
      (const v0 0)
      (if-gtz v0 :tru)
      (throw v0)
      (.try_end foo)

      (.catch (foo))
      (const v1 3)
      (return v1)

      (:tru)
      (const v2 2)
      (return v2)

      (return v0)
    )
  )";
  auto code = assembler::ircode_from_string(s_insns);
  auto code_clone = assembler::ircode_from_string(s_insns);

  IRList::iterator code_it = code->begin();
  IRList::iterator clone_it = code_clone->begin();

  while (code_it != code->end() && clone_it != code_clone->end()) {
    EXPECT_TRUE(*code_it == *clone_it);

    code_it++;
    clone_it++;
  }

  always_assert(code_it == code->end() && clone_it == code_clone->end());
}

TEST_F(IRListTest, remove_prologue) {
  const auto* const s_insns = R"(
    (
      (load-param v0)
      (.dbg DBG_SET_PROLOGUE_END)
      (const v1 1)
      (return-void)
    )
  )";
  const auto* const expected_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (return-void)
    )
  )";

  auto code = assembler::ircode_from_string(s_insns);
  auto expected_code = assembler::ircode_from_string(expected_str);

  code->cleanup_debug();

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(code.get()));
}

TEST_F(IRListTest, remove_when_register_not_used) {
  const auto* const s_insns = R"(
    (
      (load-param v0)
      (const v1 1)
      (.dbg DBG_END_LOCAL 3)
      (.dbg DBG_RESTART_LOCAL 6)
      (return-void)
    )
  )";
  auto code = assembler::ircode_from_string(s_insns);

  const auto* const expected_str = R"(
    (
      (load-param v0)
      (const v1 1)
      (return-void)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);

  code->cleanup_debug();

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(code.get()));
}

TEST_F(IRListTest, estimate_code_units_fill_array_data) {
  const auto* const s_insns = R"(
    (
      (const v0 3)
      (new-array v0 "[I")
      (move-result-pseudo-object v1)
      (fill-array-data v1 #4 (63 64 65))
      (return-void)
    )
  )";
  auto code = assembler::ircode_from_string(s_insns);

  uint32_t opcode_code_units{0};
  const DexOpcodeData* payload{nullptr};
  for (const auto& mie : *code) {
    if (mie.type != MFLOW_OPCODE) {
      continue;
    }
    opcode_code_units += mie.insn->size();
    if (opcode::is_fill_array_data(mie.insn->opcode())) {
      payload = mie.insn->get_data();
    }
  }
  ASSERT_NE(payload, nullptr);

  // 3 elements of 4 bytes = 6 code units of data, plus the 4 header code units
  // (ident, element_width, and the two size words) that DexOpcodeData::size()
  // already accounts for.
  EXPECT_EQ(payload->size(), 10);
  EXPECT_EQ(code->estimate_code_units(), opcode_code_units + payload->size());

  // Every in-pass caller goes through the CFG, which must agree.
  code->build_cfg();
  EXPECT_EQ(code->estimate_code_units(), opcode_code_units + payload->size());
}

TEST_F(IRListTest, keep_valid_regs) {
  const auto* const s_insns = R"(
    (
      (load-param v0)
      (.dbg DBG_START_LOCAL_EXTENDED 4 "will_not_be_removed" "Ljava/lang/Objects;" "sig")
      (const v1 1)
      (.dbg DBG_END_LOCAL 4)
      (return-void)
    )
  )";
  auto code = assembler::ircode_from_string(s_insns);

  const auto* const expected_str = R"(
    (
      (load-param v0)
      (.dbg DBG_START_LOCAL_EXTENDED 4 "will_not_be_removed" "Ljava/lang/Objects;" "sig")
      (const v1 1)
      (.dbg DBG_END_LOCAL 4)
      (return-void)
    )
  )";
  auto expected_code = assembler::ircode_from_string(expected_str);

  code->cleanup_debug();

  EXPECT_EQ(assembler::to_string(expected_code.get()),
            assembler::to_string(code.get()));
}
