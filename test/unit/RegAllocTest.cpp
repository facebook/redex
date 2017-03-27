/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexAsm.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "OpcodeList.h"
#include "RegAlloc.h"
#include "Show.h"
#include "Transform.h"
#include "Util.h"

#include <gtest/gtest.h>

// for nicer gtest error messages
std::ostream& operator<<(std::ostream& os, const IRInstruction& to_show) {
  return os << show(&to_show);
}

std::ostream& operator<<(std::ostream& os, const IRTypeInstruction& to_show) {
  return os << show(&to_show);
}

std::ostream& operator<<(std::ostream& os,
                         const HighRegMoveInserter::SwapInfo& info) {
  return os << "SwapInfo(" << info.low_reg_swap << ", " << info.range_swap
            << ")";
}

struct RegAllocTest : testing::Test {
  RegAllocTest() {
    g_redex = new RedexContext();
  }

  ~RegAllocTest() {
    delete g_redex;
  }
};

class InstructionList {
  std::vector<std::unique_ptr<IRInstruction>> m_insns;
 public:
  InstructionList(std::initializer_list<IRInstruction*> insns) {
    for (auto insn : insns) {
      m_insns.emplace_back(insn);
    }
  }
  ::testing::AssertionResult matches(InstructionIterable ii) {
    auto it = ii.begin();
    auto end = ii.end();
    auto match_it = m_insns.begin();
    auto match_end = m_insns.end();
    auto idx {0};
    for (; it != end && match_it != match_end; ++it, ++match_it) {
      if (*it->insn != **match_it) {
        return ::testing::AssertionFailure() << "Expected " << show(&**match_it)
                                             << " at index " << idx << ", got "
                                             << show(it->insn);
      }
      ++idx;
    }
    if (it == end && match_it == match_end) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << "Length mismatch";
  }
};

TEST_F(RegAllocTest, MoveGen) {
  using namespace dex_asm;
  // check that we pick the smallest possible instruction
  EXPECT_EQ(*gen_move(RegisterKind::OBJECT, 15, 15),
            *dasm(OPCODE_MOVE_OBJECT, {15_v, 15_v}));
  EXPECT_EQ(*gen_move(RegisterKind::OBJECT, 254, 65535),
            *dasm(OPCODE_MOVE_OBJECT_FROM16, {254_v, 65535_v}));
  EXPECT_EQ(*gen_move(RegisterKind::OBJECT, 65535, 65535),
            *dasm(OPCODE_MOVE_OBJECT_16, {65535_v, 65535_v}));
  // check that we pick the right type too
  EXPECT_EQ(*gen_move(RegisterKind::NORMAL, 15, 15),
            *dasm(OPCODE_MOVE, {15_v, 15_v}));
  EXPECT_EQ(*gen_move(RegisterKind::WIDE, 254, 65535),
            *dasm(OPCODE_MOVE_WIDE_FROM16, {254_v, 65535_v}));
}

TEST_F(RegAllocTest, RegKindWide) {
  // check for consistency...
  for (auto op : all_opcodes) {
    auto insn = std::make_unique<DexInstruction>(op);
    if (insn->dests_size() && !insn->dest_is_src()) {
      EXPECT_EQ((new IRInstruction(op))->dest_is_wide(),
                dest_kind(op) == RegisterKind::WIDE)
          << "mismatch for " << show(op);
    }
  }
}

TEST_F(RegAllocTest, InsertMoveDest) {
  using namespace dex_asm;
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "InsertMoveDest", "V", {});
  method->make_concrete(ACC_STATIC, std::make_unique<DexCode>(), false);
  method->get_code()->balloon();
  method->get_code()->set_registers_size(18);
  method->get_code()->set_ins_size(0);
  auto mt = method->get_code()->get_entries();
  auto array_ty = DexType::make_type("[I");
  mt->push_back(dasm(OPCODE_CONST_4, {0_v, 1_L}));
  mt->push_back(dasm(OPCODE_NEW_ARRAY, array_ty, {18_v, 1_v}));
  mt->push_back(dasm(OPCODE_RETURN_VOID));

  HighRegMoveInserter move_inserter;
  auto swap_info = HighRegMoveInserter::reserve_swap(method);
  EXPECT_EQ(swap_info, HighRegMoveInserter::SwapInfo(1, 0));
  move_inserter.insert_moves(method, swap_info);

  InstructionList expected_insns {
    dasm(OPCODE_CONST_4, {1_v, 1_L}),
    dasm(OPCODE_NEW_ARRAY, array_ty, {0_v, 2_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {19_v, 0_v}),
    dasm(OPCODE_RETURN_VOID)
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(mt)));
}

TEST_F(RegAllocTest, InsertMoveSrc) {
  using namespace dex_asm;
  std::vector<const char*> arg_types(17, "Ljava/lang/Object;");
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "InsertMoveSrc", "V", arg_types);
  method->make_concrete(ACC_STATIC, std::make_unique<DexCode>(), false);
  method->get_code()->balloon();
  method->get_code()->set_registers_size(17);
  method->get_code()->set_ins_size(17);
  auto mt = method->get_code()->get_entries();
  auto if_ = new MethodItemEntry(dasm(OPCODE_IF_EQ, {15_v, 16_v}));
  mt->push_back(*if_);
  mt->push_back(dasm(OPCODE_INSTANCE_OF, get_object_type(), {0_v, 15_v}));
  mt->push_back(dasm(OPCODE_RETURN_VOID));
  auto target = new BranchTarget();
  target->type = BRANCH_SIMPLE;
  target->src = if_;
  mt->push_back(target);
  mt->push_back(dasm(OPCODE_RETURN_VOID));

  HighRegMoveInserter move_inserter;
  auto swap_info = HighRegMoveInserter::reserve_swap(method);
  EXPECT_EQ(swap_info, HighRegMoveInserter::SwapInfo(2, 0));
  move_inserter.insert_moves(method, swap_info);

  InstructionList expected_insns {
    dasm(OPCODE_MOVE_OBJECT_FROM16, {0_v, 17_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {1_v, 18_v}),
    dasm(OPCODE_IF_EQ, {0_v, 1_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {0_v, 17_v}),
    dasm(OPCODE_INSTANCE_OF, get_object_type(), {2_v, 0_v}),
    dasm(OPCODE_RETURN_VOID),
    dasm(OPCODE_RETURN_VOID)
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(mt)));
}

TEST_F(RegAllocTest, ConvertToRangeForOneSrc) {
  using namespace dex_asm;
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "ConvertToRangeForOneSrc", "V", {});
  method->make_concrete(ACC_STATIC, std::make_unique<DexCode>(), false);
  method->get_code()->balloon();
  method->get_code()->set_registers_size(17);
  method->get_code()->set_ins_size(0);
  auto mt = method->get_code()->get_entries();
  mt->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {16_v}));
  // can be converted to invoke-virtual/range without needing to move the
  // register
  mt->push_back(
      dasm(OPCODE_INVOKE_VIRTUAL,
           DexMethod::make_method("Ljava/lang/Object", "hashCode", "V", {}),
           {16_v}));
  mt->push_back(dasm(OPCODE_RETURN_VOID));

  HighRegMoveInserter move_inserter;
  auto swap_info = HighRegMoveInserter::reserve_swap(method);
  EXPECT_EQ(swap_info, HighRegMoveInserter::SwapInfo(0, 0));
  move_inserter.insert_moves(method, swap_info);

  InstructionList expected_insns {
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {16_v}),
    dasm(OPCODE_INVOKE_VIRTUAL_RANGE,
         DexMethod::make_method("Ljava/lang/Object", "hashCode", "V", {}))
      ->set_range_base(16)->set_range_size(1),
    dasm(OPCODE_RETURN_VOID)
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(mt)));
}

TEST_F(RegAllocTest, ConvertToRangeForTwoSrc) {
  using namespace dex_asm;
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "ConvertToRangeForTwoSrc", "V", {});
  method->make_concrete(ACC_STATIC, std::make_unique<DexCode>(), false);
  method->get_code()->balloon();
  method->get_code()->set_registers_size(19);
  method->get_code()->set_ins_size(0);
  auto mt = method->get_code()->get_entries();
  mt->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {16_v}));
  mt->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {17_v}));
  // the register args to invoke don't map directly to a range, so we'll insert
  // move opcodes to shuffle them into place
  mt->push_back(
      dasm(OPCODE_INVOKE_VIRTUAL,
           DexMethod::make_method(
               "Ljava/lang/Object", "equals", "B", {"Ljava/lang/Object"}),
           {17_v, 16_v}));
  mt->push_back(dasm(OPCODE_RETURN_VOID));

  HighRegMoveInserter move_inserter;
  auto swap_info = HighRegMoveInserter::reserve_swap(method);
  EXPECT_EQ(swap_info, HighRegMoveInserter::SwapInfo(0, 2));
  move_inserter.insert_moves(method, swap_info);

  InstructionList expected_insns {
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {16_v}),
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {17_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {19_v, 17_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {20_v, 16_v}),
    dasm(OPCODE_INVOKE_VIRTUAL_RANGE,
         DexMethod::make_method("Ljava/lang/Object", "equals", "B",
           {"Ljava/lang/Object"}))
      ->set_range_base(19)->set_range_size(2),
    dasm(OPCODE_RETURN_VOID)
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(mt)));
}
