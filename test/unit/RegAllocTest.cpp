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
#include "LiveRange.h"
#include "OpcodeList.h"
#include "RegAlloc.h"
#include "RegisterType.h"
#include "Show.h"
#include "Transform.h"
#include "Util.h"

#include <gtest/gtest.h>

using namespace regalloc;

// for nicer gtest error messages
std::ostream& operator<<(std::ostream& os, const IRInstruction& to_show) {
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

/*
 * Check that we pick the most pessimistic move instruction (of the right type)
 * that can address arbitrarily large registers -- we will shrink it down later
 * as necessary when syncing the IRCode.
 */
TEST_F(RegAllocTest, MoveGen) {
  using namespace dex_asm;
  EXPECT_EQ(*gen_move(RegisterType::NORMAL, 1, 2),
            *dasm(OPCODE_MOVE_16, {1_v, 2_v}));
  EXPECT_EQ(*gen_move(RegisterType::ZERO, 1, 2),
            *dasm(OPCODE_MOVE_16, {1_v, 2_v}));
  EXPECT_EQ(*gen_move(RegisterType::OBJECT, 1, 2),
            *dasm(OPCODE_MOVE_OBJECT_16, {1_v, 2_v}));
  EXPECT_EQ(*gen_move(RegisterType::WIDE, 1, 2),
            *dasm(OPCODE_MOVE_WIDE_16, {1_v, 2_v}));
}

TEST_F(RegAllocTest, RegTypeDestWide) {
  // check for consistency...
  for (auto op : all_opcodes) {
    if (opcode_impl::dests_size(op) && !opcode::dest_is_src(op)) {
      auto insn = std::make_unique<IRInstruction>(op);
      EXPECT_EQ(insn->dest_is_wide(),
                regalloc::dest_reg_type(insn.get()) == RegisterType::WIDE)
          << "mismatch for " << show(op);
    }
  }
}

/*
 * Check that we infer the correct register type for static and non-static
 * invoke instructions.
 */
TEST_F(RegAllocTest, RegTypeInvoke) {
  using namespace dex_asm;
  DexMethod* method = DexMethod::make_method("Lfoo;", "bar", "V", {"I"});

  auto insn = dasm(OPCODE_INVOKE_DIRECT, method, {0_v, 1_v});
  EXPECT_EQ(regalloc::src_reg_type(insn, 0), RegisterType::OBJECT);
  EXPECT_EQ(regalloc::src_reg_type(insn, 1), RegisterType::NORMAL);

  auto static_insn = dasm(OPCODE_INVOKE_STATIC, method, {0_v});
  EXPECT_EQ(regalloc::src_reg_type(static_insn, 0), RegisterType::NORMAL);
}

TEST_F(RegAllocTest, InsertMoveDest) {
  using namespace dex_asm;
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "InsertMoveDest", "V", {});
  method->make_concrete(ACC_STATIC, false);
  method->set_code(std::make_unique<IRCode>(method, 18));
  auto* code = method->get_code();
  auto array_ty = DexType::make_type("[I");
  code->push_back(dasm(OPCODE_CONST_4, {0_v, 1_L}));
  code->push_back(dasm(OPCODE_NEW_ARRAY, array_ty, {18_v, 1_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  HighRegMoveInserter move_inserter;
  auto swap_info = HighRegMoveInserter::reserve_swap(method);
  EXPECT_EQ(swap_info, HighRegMoveInserter::SwapInfo(1, 0));
  move_inserter.insert_moves(code, swap_info);

  InstructionList expected_insns {
    dasm(OPCODE_CONST_4, {1_v, 1_L}),
    dasm(OPCODE_NEW_ARRAY, array_ty, {0_v, 2_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {19_v, 0_v}),
    dasm(OPCODE_RETURN_VOID)
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}

TEST_F(RegAllocTest, InsertMoveSrc) {
  using namespace dex_asm;
  std::vector<const char*> arg_types(17, "Ljava/lang/Object;");
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "InsertMoveSrc", "V", arg_types);
  method->make_concrete(ACC_STATIC, false);
  method->set_code(std::make_unique<IRCode>(method, 0));
  auto* code = method->get_code();
  auto if_ = new MethodItemEntry(dasm(OPCODE_IF_EQ, {15_v, 16_v}));
  code->push_back(*if_);
  code->push_back(dasm(OPCODE_INSTANCE_OF, get_object_type(), {0_v, 15_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));
  auto target = new BranchTarget();
  target->type = BRANCH_SIMPLE;
  target->src = if_;
  code->push_back(target);
  code->push_back(dasm(OPCODE_RETURN_VOID));

  HighRegMoveInserter move_inserter;
  auto swap_info = HighRegMoveInserter::reserve_swap(method);
  EXPECT_EQ(swap_info, HighRegMoveInserter::SwapInfo(2, 0));
  move_inserter.insert_moves(code, swap_info);

  InstructionList expected_insns {
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {2_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {3_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {4_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {5_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {6_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {7_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {8_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {9_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {10_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {11_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {12_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {13_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {14_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {15_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {16_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {17_v}),
    dasm(IOPCODE_LOAD_PARAM_OBJECT, {18_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {0_v, 17_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {1_v, 18_v}),
    dasm(OPCODE_IF_EQ, {0_v, 1_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {0_v, 17_v}),
    dasm(OPCODE_INSTANCE_OF, get_object_type(), {2_v, 0_v}),
    dasm(OPCODE_RETURN_VOID),
    dasm(OPCODE_RETURN_VOID)
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}

TEST_F(RegAllocTest, ConvertToRangeForOneSrc) {
  using namespace dex_asm;
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "ConvertToRangeForOneSrc", "V", {});
  method->make_concrete(ACC_STATIC, false);
  method->set_code(std::make_unique<IRCode>(method, 17));
  auto* code = method->get_code();
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {16_v}));
  // can be converted to invoke-virtual/range without needing to move the
  // register
  code->push_back(
      dasm(OPCODE_INVOKE_VIRTUAL,
           DexMethod::make_method("Ljava/lang/Object", "hashCode", "V", {}),
           {16_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  HighRegMoveInserter move_inserter;
  auto swap_info = HighRegMoveInserter::reserve_swap(method);
  EXPECT_EQ(swap_info, HighRegMoveInserter::SwapInfo(0, 0));
  move_inserter.insert_moves(code, swap_info);

  InstructionList expected_insns{
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {16_v}),
    dasm(OPCODE_INVOKE_VIRTUAL,
         DexMethod::make_method("Ljava/lang/Object", "hashCode", "V", {}),
         {16_v}),
    dasm(OPCODE_RETURN_VOID)
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}

TEST_F(RegAllocTest, ConvertToRangeForTwoSrc) {
  using namespace dex_asm;
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "ConvertToRangeForTwoSrc", "V", {});
  method->make_concrete(ACC_STATIC, false);
  method->set_code(std::make_unique<IRCode>(method, 19));
  auto* code = method->get_code();
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {16_v}));
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {17_v}));
  // the register args to invoke don't map directly to a range, so we'll insert
  // move opcodes to shuffle them into place
  code->push_back(
      dasm(OPCODE_INVOKE_VIRTUAL,
           DexMethod::make_method(
               "Ljava/lang/Object", "equals", "B", {"Ljava/lang/Object"}),
           {17_v, 16_v}));
  code->push_back(dasm(OPCODE_RETURN_VOID));

  HighRegMoveInserter move_inserter;
  auto swap_info = HighRegMoveInserter::reserve_swap(method);
  EXPECT_EQ(swap_info, HighRegMoveInserter::SwapInfo(0, 2));
  move_inserter.insert_moves(code, swap_info);

  InstructionList expected_insns {
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {16_v}),
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {17_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {19_v, 17_v}),
    dasm(OPCODE_MOVE_OBJECT_FROM16, {20_v, 16_v}),
    dasm(OPCODE_INVOKE_VIRTUAL,
         DexMethod::make_method(
             "Ljava/lang/Object", "equals", "B", {"Ljava/lang/Object"}),
         {19_v, 20_v}),
    dasm(OPCODE_RETURN_VOID)
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}

TEST_F(RegAllocTest, LiveRangeSingleBlock) {
  using namespace dex_asm;
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "LiveRangeSingleBlock", "V", {});
  method->make_concrete(ACC_STATIC, false);
  method->set_code(std::make_unique<IRCode>(method, 1));
  auto code = method->get_code();
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}));
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}));
  code->push_back(dasm(OPCODE_CHECK_CAST, get_object_type(), {0_v, 0_v}));

  live_range::renumber_registers(code);

  InstructionList expected_insns {
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}),
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {1_v}),
    dasm(OPCODE_CHECK_CAST, get_object_type(), {2_v, 1_v}),
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
  EXPECT_EQ(code->get_registers_size(), 3);
}

TEST_F(RegAllocTest, LiveRange) {
  using namespace dex_asm;
  DexMethod* method =
      DexMethod::make_method("Lfoo;", "LiveRange", "V", {});
  method->make_concrete(ACC_STATIC, false);
  method->set_code(std::make_unique<IRCode>(method, 1));
  auto code = method->get_code();
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}));
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}));
  code->push_back(dasm(OPCODE_CHECK_CAST, get_object_type(), {0_v, 0_v}));
  auto if_ = new MethodItemEntry(dasm(OPCODE_IF_EQ, {0_v, 0_v}));
  code->push_back(*if_);

  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}));
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}));
  code->push_back(dasm(OPCODE_CHECK_CAST, get_object_type(), {0_v, 0_v}));
  auto target = new BranchTarget();
  target->type = BRANCH_SIMPLE;
  target->src = if_;
  code->push_back(target);

  code->push_back(dasm(OPCODE_CHECK_CAST, get_object_type(), {0_v, 0_v}));

  live_range::renumber_registers(code);

  InstructionList expected_insns {
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}),
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {1_v}),
    dasm(OPCODE_CHECK_CAST, get_object_type(), {2_v, 1_v}),
    dasm(OPCODE_IF_EQ, {2_v, 2_v}),
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {3_v}),
    dasm(OPCODE_NEW_INSTANCE, get_object_type(), {4_v}),
    dasm(OPCODE_CHECK_CAST, get_object_type(), {2_v, 4_v}),
    // target of if-eq
    dasm(OPCODE_CHECK_CAST, get_object_type(), {5_v, 2_v}),
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
  EXPECT_EQ(code->get_registers_size(), 6);
}
