/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cmath>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexUtil.h"
#include "GraphColoring.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Interference.h"
#include "LiveRange.h"
#include "Liveness.h"
#include "OpcodeList.h"
#include "RegAlloc.h"
#include "RegisterType.h"
#include "Show.h"
#include "Transform.h"
#include "Util.h"
#include "VirtualRegistersFile.h"

using namespace regalloc;

// for nicer gtest error messages
std::ostream& operator<<(std::ostream& os, const IRInstruction& to_show) {
  return os << show(&to_show);
}

struct RegAllocTest : testing::Test {
  RegAllocTest() { g_redex = new RedexContext(); }

  ~RegAllocTest() { delete g_redex; }
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
  DexMethodRef* method = DexMethod::make_method("Lfoo;", "bar", "V", {"I"});

  auto insn = dasm(OPCODE_INVOKE_DIRECT, method, {0_v, 1_v});
  EXPECT_EQ(regalloc::src_reg_type(insn, 0), RegisterType::OBJECT);
  EXPECT_EQ(regalloc::src_reg_type(insn, 1), RegisterType::NORMAL);

  auto static_insn = dasm(OPCODE_INVOKE_STATIC, method, {0_v});
  EXPECT_EQ(regalloc::src_reg_type(static_insn, 0), RegisterType::NORMAL);
}

TEST_F(RegAllocTest, LiveRangeSingleBlock) {
  using namespace dex_asm;
  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "LiveRangeSingleBlock", "V", {}));
  method->make_concrete(ACC_STATIC, false);
  method->set_code(std::make_unique<IRCode>(method, 1));
  auto code = method->get_code();
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}));
  code->push_back(dasm(OPCODE_NEW_INSTANCE, get_object_type(), {0_v}));
  code->push_back(dasm(OPCODE_CHECK_CAST, get_object_type(), {0_v, 0_v}));

  code->build_cfg();
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
  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "LiveRange", "V", {}));
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

  code->build_cfg();
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

TEST_F(RegAllocTest, VirtualRegistersFile) {
  VirtualRegistersFile vreg_file;
  auto to_string = [](const VirtualRegistersFile& vreg_file) {
    std::ostringstream ss;
    ss << vreg_file;
    return ss.str();
  };

  // check edge case where the register file is empty
  EXPECT_EQ(to_string(vreg_file), "");
  EXPECT_TRUE(vreg_file.is_free(0, 2));
  EXPECT_TRUE(vreg_file.is_free(1, 2));

  EXPECT_EQ(vreg_file.alloc(1), 0);
  EXPECT_EQ(vreg_file.alloc(2), 1);
  EXPECT_EQ(vreg_file.alloc(1), 3);
  // Current state (`!` means allocated):
  EXPECT_EQ(to_string(vreg_file), "!0 !1 !2 !3");

  // check that we take advantage of "holes" in the register file
  vreg_file.free(1, 2);
  EXPECT_EQ(to_string(vreg_file), "!0  1  2 !3");
  EXPECT_TRUE(vreg_file.is_free(1, 1));

  EXPECT_EQ(vreg_file.alloc(1), 1);
  EXPECT_EQ(to_string(vreg_file), "!0 !1  2 !3");
  EXPECT_FALSE(vreg_file.is_free(1, 2));

  // check that we correctly skip over the free register "hole" because it is
  // not large enough for the requested allocation size.
  EXPECT_EQ(vreg_file.alloc(2), 4);
  EXPECT_EQ(to_string(vreg_file), "!0 !1  2 !3 !4 !5");
  EXPECT_EQ(vreg_file.size(), 6);

  // check that we handle edge case correctly -- when some free space is at the
  // end of the file, but insufficient for the full width requested
  vreg_file.free(5, 1);
  EXPECT_EQ(to_string(vreg_file), "!0 !1  2 !3 !4  5");
  // half of the register pair is past the end of the frame, but it should not
  // matter
  EXPECT_TRUE(vreg_file.is_free(5, 2));
  EXPECT_EQ(vreg_file.alloc(2), 5);
  EXPECT_EQ(to_string(vreg_file), "!0 !1  2 !3 !4 !5 !6");
  EXPECT_EQ(vreg_file.size(), 7);

  // check the case where there is no free space at all at the end of the file

  // 7 is beyond the end of the current frame, but it should not matter
  EXPECT_TRUE(vreg_file.is_free(7, 2));
  vreg_file.alloc_at(7, 2);
  EXPECT_EQ(to_string(vreg_file), "!0 !1  2 !3 !4 !5 !6 !7 !8");
  EXPECT_EQ(vreg_file.size(), 9);
}

TEST_F(RegAllocTest, InterferenceWeights) {
  using namespace interference::impl;
  // Check that our div_ceil implementation is consistent with the more
  // obviously correct alternative of converting to a double before dividing
  auto fp_div_ceil = [](double x, double y) -> uint32_t { return ceil(x / y); };
  for (uint8_t width = 1; width < 2; ++width) {
    // This is the calculation for colorable_limit()
    EXPECT_EQ(div_ceil(max_unsigned_value(16) + 1, 2 * width - 1),
              fp_div_ceil(max_unsigned_value(16) + 1, 2 * width - 1));
  }

  // Check that our optimized edge_weight calculation is consistent with the
  // slower division-based method
  EXPECT_EQ(fp_div_ceil(1, 1), edge_weight(1, 1));
  EXPECT_EQ(fp_div_ceil(1, 2), edge_weight(2, 1));
  EXPECT_EQ(fp_div_ceil(2, 1), edge_weight(1, 2));
  EXPECT_EQ(fp_div_ceil(2, 2), edge_weight(2, 2));
}

TEST_F(RegAllocTest, BuildInterferenceGraph) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "I", {"I", "I"}));
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  EXPECT_EQ(*code.begin()->insn, *dasm(IOPCODE_LOAD_PARAM, {0_v}));
  EXPECT_EQ(*std::next(code.begin())->insn, *dasm(IOPCODE_LOAD_PARAM, {1_v}));
  code.push_back(dasm(OPCODE_CONST_4, {2_v}));
  code.push_back(dasm(OPCODE_ADD_INT, {3_v, 0_v, 2_v}));
  code.push_back(dasm(OPCODE_RETURN, {3_v}));
  code.set_registers_size(4);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);
  // +---+
  // | 1 |
  // +---+
  //   |
  // +---+     +---+  +---+
  // | 0 | --- | 2 |  | 3 |
  // +---+     +---+  +---+
  EXPECT_EQ(ig.nodes().size(), 4);
  EXPECT_EQ(ig.get_node(0).max_vreg(), 255);
  EXPECT_THAT(ig.get_node(0).adjacent(), ::testing::UnorderedElementsAre(1, 2));
  EXPECT_EQ(ig.get_node(0).type(), RegisterType::NORMAL);
  EXPECT_EQ(ig.get_node(1).max_vreg(), 65535);
  EXPECT_EQ(ig.get_node(1).adjacent(), std::vector<reg_t>{0});
  EXPECT_EQ(ig.get_node(1).type(), RegisterType::NORMAL);
  EXPECT_EQ(ig.get_node(2).max_vreg(), 15);
  EXPECT_EQ(ig.get_node(2).adjacent(), std::vector<reg_t>{0});
  EXPECT_EQ(ig.get_node(2).type(), RegisterType::NORMAL);
  EXPECT_EQ(ig.get_node(3).max_vreg(), 255);
  EXPECT_EQ(ig.get_node(3).adjacent(), std::vector<reg_t>{});
  EXPECT_EQ(ig.get_node(3).type(), RegisterType::NORMAL);

  // Check that the adjacency matrix is consistent with the adjacency lists
  for (auto& pair : ig.nodes()) {
    auto reg = pair.first;
    for (auto adj : pair.second.adjacent()) {
      EXPECT_TRUE(ig.is_adjacent(reg, adj));
      EXPECT_TRUE(ig.is_adjacent(adj, reg));
    }
  }
}

TEST_F(RegAllocTest, CombineNonAdjacentNodes) {
  using namespace interference::impl;
  auto ig = GraphBuilder::create_empty();
  GraphBuilder::make_node(&ig, 0, RegisterType::NORMAL, /* max_vreg */ 3);
  GraphBuilder::make_node(&ig, 1, RegisterType::NORMAL, /* max_vreg */ 3);
  GraphBuilder::make_node(&ig, 2, RegisterType::NORMAL, /* max_vreg */ 3);
  GraphBuilder::make_node(&ig, 3, RegisterType::NORMAL, /* max_vreg */ 3);
  GraphBuilder::add_edge(&ig, 0, 1);
  GraphBuilder::add_edge(&ig, 0, 2);
  GraphBuilder::add_edge(&ig, 2, 3);
  // +---+
  // | 1 |
  // +---+
  //   |
  // +---+     +---+    +---+
  // | 0 | --- | 2 | -- | 3 |
  // +---+     +---+    +---+
  EXPECT_EQ(ig.get_node(0).weight(), 2);
  EXPECT_EQ(ig.get_node(1).weight(), 1);
  EXPECT_EQ(ig.get_node(2).weight(), 2);
  EXPECT_EQ(ig.get_node(3).weight(), 1);
  ig.combine(1, 2);
  EXPECT_EQ(ig.get_node(0).weight(), 1);
  EXPECT_EQ(ig.get_node(1).weight(), 2);
  EXPECT_EQ(ig.get_node(3).weight(), 1);
  EXPECT_FALSE(ig.get_node(2).is_active());
}

TEST_F(RegAllocTest, CombineAdjacentNodes) {
  using namespace interference::impl;
  auto ig = GraphBuilder::create_empty();
  GraphBuilder::make_node(&ig, 0, RegisterType::NORMAL, /* max_vreg */ 3);
  GraphBuilder::make_node(&ig, 1, RegisterType::NORMAL, /* max_vreg */ 3);
  GraphBuilder::make_node(&ig, 2, RegisterType::NORMAL, /* max_vreg */ 3);
  GraphBuilder::make_node(&ig, 3, RegisterType::NORMAL, /* max_vreg */ 3);
  GraphBuilder::add_edge(&ig, 0, 1);
  GraphBuilder::add_edge(&ig, 0, 2);
  GraphBuilder::add_edge(&ig, 2, 3);
  // +---+
  // | 1 |
  // +---+
  //   |
  // +---+     +---+    +---+
  // | 0 | --- | 2 | -- | 3 |
  // +---+     +---+    +---+
  EXPECT_EQ(ig.get_node(0).weight(), 2);
  EXPECT_EQ(ig.get_node(1).weight(), 1);
  EXPECT_EQ(ig.get_node(2).weight(), 2);
  EXPECT_EQ(ig.get_node(3).weight(), 1);
  ig.combine(0, 2);
  EXPECT_EQ(ig.get_node(0).weight(), 2);
  EXPECT_EQ(ig.get_node(1).weight(), 1);
  EXPECT_EQ(ig.get_node(3).weight(), 1);
  EXPECT_FALSE(ig.get_node(2).is_active());
}

TEST_F(RegAllocTest, Coalesce) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "I", {}));
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  code.push_back(dasm(OPCODE_CONST_4, {0_v, 0_L}));
  code.push_back(dasm(OPCODE_MOVE, {1_v, 0_v}));
  code.push_back(dasm(OPCODE_RETURN, {1_v}));
  code.set_registers_size(2);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);
  graph_coloring::Allocator allocator;
  allocator.coalesce(&ig, &code);
  InstructionList expected_insns {
    dasm(OPCODE_CONST_4, {0_v, 0_L}),
    // move opcode was coalesced
    dasm(OPCODE_RETURN, {0_v})
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}

TEST_F(RegAllocTest, MoveWideCoalesce) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "I", {}));
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  code.push_back(dasm(OPCODE_CONST_WIDE, {0_v, 0_L}));
  code.push_back(dasm(OPCODE_MOVE_WIDE, {1_v, 0_v}));
  code.push_back(dasm(OPCODE_RETURN_WIDE, {1_v}));
  code.set_registers_size(2);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);

  EXPECT_TRUE(ig.is_coalesceable(0, 1));
  EXPECT_TRUE(ig.is_adjacent(0, 1));

  graph_coloring::Allocator allocator;
  allocator.coalesce(&ig, &code);
  InstructionList expected_insns {
    dasm(OPCODE_CONST_WIDE, {0_v, 0_L}),
    // move-wide opcode was coalesced
    dasm(OPCODE_RETURN_WIDE, {0_v})
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}

TEST_F(RegAllocTest, NoCoalesceWide) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "I", {}));
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  code.push_back(dasm(OPCODE_CONST_WIDE, {0_v, 0_L}));
  code.push_back(dasm(OPCODE_MOVE_WIDE, {1_v, 0_v}));
  code.push_back(dasm(OPCODE_LONG_TO_DOUBLE, {1_v, 0_v}));
  code.push_back(dasm(OPCODE_RETURN_WIDE, {0_v}));
  code.set_registers_size(2);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);

  EXPECT_FALSE(ig.is_coalesceable(0, 1));
  EXPECT_TRUE(ig.is_adjacent(0, 1));

  graph_coloring::Allocator allocator;
  allocator.coalesce(&ig, &code);
  InstructionList expected_insns {
    dasm(OPCODE_CONST_WIDE, {0_v, 0_L}),
    // This move can't be coalesced away due to the long-to-double instruction
    // below
    dasm(OPCODE_MOVE_WIDE, {1_v, 0_v}),
    dasm(OPCODE_LONG_TO_DOUBLE, {1_v, 0_v}),
    dasm(OPCODE_RETURN_WIDE, {0_v})
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}

static std::vector<reg_t> stack_to_vec(std::stack<reg_t> stack) {
  std::vector<reg_t> vec;
  while (!stack.empty()) {
    vec.push_back(stack.top());
    stack.pop();
  }
  return vec;
}

TEST_F(RegAllocTest, Simplify) {
  using namespace interference::impl;
  auto ig = GraphBuilder::create_empty();
  // allocate in a 3-register-wide frame
  GraphBuilder::make_node(&ig, 0, RegisterType::NORMAL, /* max_vreg */ 2);
  GraphBuilder::make_node(&ig, 1, RegisterType::WIDE, /* max_vreg */ 2);
  GraphBuilder::make_node(&ig, 2, RegisterType::NORMAL, /* max_vreg */ 2);
  GraphBuilder::add_edge(&ig, 0, 1);
  GraphBuilder::add_edge(&ig, 0, 2);

  EXPECT_EQ(ig.get_node(0).weight(), 3);
  EXPECT_EQ(ig.get_node(1).weight(), 1);
  EXPECT_EQ(ig.get_node(2).weight(), 1);
  EXPECT_EQ(ig.get_node(0).colorable_limit(), 3);
  EXPECT_EQ(ig.get_node(1).colorable_limit(), 1);
  EXPECT_EQ(ig.get_node(2).colorable_limit(), 3);
  EXPECT_FALSE(ig.get_node(0).definitely_colorable());
  EXPECT_FALSE(ig.get_node(1).definitely_colorable());
  EXPECT_TRUE(ig.get_node(2).definitely_colorable());
  // +-------+
  // |   1   |
  // +-------+
  //   |
  // +---+     +---+
  // | 0 | --- | 2 |
  // +---+     +---+
  //
  // At first, only node 2 is colorable. After removing it, node 0 has weight
  // 1, so it is colorable too. Only after node 0 is removed is node 1
  // colorable. We color in reverse order of removal -- 1 0 2. To see why it
  // is necessary, suppose we colored 0 before 1 and put it in the middle:
  //
  //   [ ][0][ ]
  //
  // Now we cannot color 1.
  //
  // If we colored 1 and 2 before 0, we could end up like so:
  //
  //   [1][1][2]
  //
  // now we cannot color 0.
  graph_coloring::Allocator allocator;
  std::stack<reg_t> select_stack;
  std::stack<reg_t> spilled_select_stack;
  allocator.simplify(true, &ig, &select_stack, &spilled_select_stack);
  auto selected = stack_to_vec(select_stack);
  EXPECT_EQ(selected, std::vector<reg_t>({1, 0, 2}));
}

TEST_F(RegAllocTest, SelectRange) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(DexMethod::make_method(
      "Lfoo;", "bar", "I", {"I", "I", "I", "I", "I", "I"}));
  DexMethodRef* many_args_method = DexMethod::make_method(
      "Lfoo;", "baz", "V", {"I", "I", "I", "I", "I", "I"});
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  // the invoke instruction references the param registers in order; make sure
  // we map them 1:1 without any spills, and map v6 to the start of the frame
  // (since the params must be at the end)
  code.push_back(dasm(OPCODE_CONST_4, {6_v}));
  code.push_back(dasm(
      OPCODE_INVOKE_STATIC, many_args_method, {0_v, 1_v, 2_v, 3_v, 4_v, 5_v}));
  code.push_back(dasm(OPCODE_ADD_INT, {3_v, 0_v, 6_v}));
  code.push_back(dasm(OPCODE_RETURN, {3_v}));
  code.set_registers_size(7);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set = init_range_set(&code);
  EXPECT_EQ(range_set.size(), 1);
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);
  for (size_t i = 0; i < 6; ++i) {
    auto& node = ig.get_node(i);
    EXPECT_TRUE(node.is_range() && node.is_param());
  }
  EXPECT_FALSE(ig.get_node(6).is_range());

  graph_coloring::SpillPlan spill_plan;
  graph_coloring::RegisterTransform reg_transform;
  graph_coloring::Allocator allocator;
  std::stack<reg_t> select_stack;
  std::stack<reg_t> spilled_select_stack;
  allocator.simplify(true, &ig, &select_stack, &spilled_select_stack);
  allocator.select(&code, ig, &select_stack, &reg_transform, &spill_plan);
  // v3 is referenced by both range and non-range instructions. We should not
  // allocate it in select() but leave it to select_ranges()
  EXPECT_EQ(reg_transform.map, (transform::RegMap{{6, 0}}));
  allocator.select_ranges(&code, ig, range_set, &reg_transform, &spill_plan);
  EXPECT_EQ(reg_transform.map,
            (transform::RegMap{
                {0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 0}}));
  EXPECT_EQ(reg_transform.size, 7);
  EXPECT_TRUE(spill_plan.empty());
}

TEST_F(RegAllocTest, SelectAliasedRange) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "V", {}));
  DexMethodRef* range_callee =
      DexMethod::make_method("Lfoo;", "baz", "V", {"I", "I"});
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  code.push_back(dasm(OPCODE_CONST_4, {0_v}));
  auto* invoke = dasm(OPCODE_INVOKE_STATIC, range_callee, {0_v, 0_v});
  code.push_back(invoke);
  code.push_back(dasm(OPCODE_RETURN_VOID));
  code.set_registers_size(2);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  range_set.emplace(invoke);
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);
  graph_coloring::SpillPlan spill_plan;
  graph_coloring::RegisterTransform reg_transform;
  graph_coloring::Allocator allocator;
  allocator.select_ranges(&code, ig, range_set, &reg_transform, &spill_plan);

  EXPECT_EQ(spill_plan.range_spills.at(invoke), std::unordered_set<reg_t>{0});
}

TEST_F(RegAllocTest, Spill) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "I", {}));
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  code.push_back(dasm(OPCODE_CONST_4, {0_v, 1_L}));
  code.push_back(dasm(OPCODE_CONST_4, {1_v, 1_L}));
  code.push_back(dasm(OPCODE_ADD_INT, {2_v, 0_v, 1_v}));
  code.push_back(dasm(OPCODE_RETURN, {2_v}));
  code.set_registers_size(3);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);

  SplitPlan split_plan;
  graph_coloring::SpillPlan spill_plan;
  spill_plan.global_spills = std::unordered_map<reg_t, reg_t> {
    {0, 16},
    {1, 16},
    {2, 256},
  };
  std::unordered_set<reg_t> new_temps;
  graph_coloring::Allocator allocator;
  allocator.spill(ig, spill_plan, range_set, &code, &new_temps);

  InstructionList expected_insns {
    dasm(OPCODE_CONST_4, {3_v, 1_L}),
    dasm(OPCODE_MOVE_16, {0_v, 3_v}),
    dasm(OPCODE_CONST_4, {4_v, 1_L}),
    dasm(OPCODE_MOVE_16, {1_v, 4_v}),

    // srcs not spilled -- add-int can address up to 8-bit-sized operands
    dasm(OPCODE_ADD_INT, {5_v, 0_v, 1_v}),
    dasm(OPCODE_MOVE_16, {2_v, 5_v}),

    dasm(OPCODE_MOVE_16, {6_v, 2_v}),
    dasm(OPCODE_RETURN, {6_v})
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}

TEST_F(RegAllocTest, ContainmentGraph) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "I", {"I", "I"}));
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  EXPECT_EQ(*code.begin()->insn, *dasm(IOPCODE_LOAD_PARAM, {0_v}));
  EXPECT_EQ(*std::next(code.begin())->insn, *dasm(IOPCODE_LOAD_PARAM, {1_v}));
  code.push_back(dasm(OPCODE_MOVE, {2_v, 0_v}));
  code.push_back(dasm(OPCODE_MOVE, {3_v, 1_v}));
  code.push_back(dasm(OPCODE_ADD_INT, {4_v, 2_v, 3_v}));
  code.push_back(dasm(OPCODE_RETURN, {4_v}));
  code.set_registers_size(5);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);
  EXPECT_TRUE(ig.has_containment_edge(0, 1));
  EXPECT_TRUE(ig.has_containment_edge(1, 0));
  EXPECT_TRUE(ig.has_containment_edge(1, 2));
  EXPECT_TRUE(ig.has_containment_edge(2, 1));
  EXPECT_TRUE(ig.has_containment_edge(3, 2));

  EXPECT_FALSE(ig.has_containment_edge(4, 2));
  EXPECT_FALSE(ig.has_containment_edge(2, 4));
  EXPECT_FALSE(ig.has_containment_edge(4, 3));
  EXPECT_FALSE(ig.has_containment_edge(3, 4));

  EXPECT_FALSE(ig.has_containment_edge(0, 4));
  EXPECT_FALSE(ig.has_containment_edge(1, 4));
  EXPECT_FALSE(ig.has_containment_edge(4, 0));
  EXPECT_FALSE(ig.has_containment_edge(4, 1));

  graph_coloring::Allocator allocator;
  allocator.coalesce(&ig, &code);
  InstructionList expected_insns{dasm(IOPCODE_LOAD_PARAM, {0_v}),
                                 dasm(IOPCODE_LOAD_PARAM, {1_v}),
                                 // move opcode was coalesced
                                 dasm(OPCODE_ADD_INT, {0_v, 0_v, 1_v}),
                                 dasm(OPCODE_RETURN, {0_v})};
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
  EXPECT_TRUE(ig.has_containment_edge(1, 0));
  EXPECT_TRUE(ig.has_containment_edge(0, 1));
}

TEST_F(RegAllocTest, FindSplit) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "I", {}));
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  code.push_back(dasm(OPCODE_CONST_4, {0_v, 1_L}));
  code.push_back(dasm(OPCODE_CONST_4, {1_v, 1_L}));
  code.push_back(dasm(OPCODE_MOVE, {2_v, 1_v}));
  code.push_back(dasm(OPCODE_MOVE, {4_v, 1_v}));
  code.push_back(dasm(OPCODE_MOVE, {3_v, 0_v}));
  code.push_back(dasm(OPCODE_RETURN, {3_v}));
  code.set_registers_size(5);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);

  SplitCosts split_costs;
  SplitPlan split_plan;
  graph_coloring::SpillPlan spill_plan;
  spill_plan.global_spills = std::unordered_map<reg_t, reg_t>{{1, 16}};
  graph_coloring::RegisterTransform reg_transform;
  reg_transform.map = transform::RegMap{{0, 0}, {2, 1}, {4, 1}, {3, 1}};
  graph_coloring::Allocator allocator;
  allocator.spill_costs(&code, ig, range_set, &spill_plan);
  calc_split_costs(fixpoint_iter, &code, &split_costs);
  allocator.find_split(
      ig, split_costs, &reg_transform, &spill_plan, &split_plan);
  EXPECT_EQ(split_plan.split_around.at(1), std::unordered_set<reg_t>{0});
}

TEST_F(RegAllocTest, Split) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "I", {}));
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  code.push_back(dasm(OPCODE_CONST_4, {0_v, 1_L}));
  code.push_back(dasm(OPCODE_CONST_4, {1_v, 1_L}));
  code.push_back(dasm(OPCODE_MOVE, {2_v, 1_v}));
  code.push_back(dasm(OPCODE_MOVE, {4_v, 1_v}));
  code.push_back(dasm(OPCODE_MOVE, {3_v, 0_v}));
  code.push_back(dasm(OPCODE_RETURN, {3_v}));
  code.set_registers_size(5);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);

  SplitCosts split_costs;
  SplitPlan split_plan;
  graph_coloring::SpillPlan spill_plan;
  // split 0 around 1
  split_plan.split_around =
      std::unordered_map<reg_t, std::unordered_set<reg_t>>{
          {1, std::unordered_set<reg_t>{0}}};
  graph_coloring::Allocator allocator;
  std::unordered_set<reg_t> new_temps;
  allocator.spill(ig, spill_plan, range_set, &code, &new_temps);
  split(fixpoint_iter, split_plan, split_costs, ig, &code);
  InstructionList expected_insns{dasm(OPCODE_CONST_4, {0_v, 1_L}),
                                 dasm(OPCODE_MOVE_16, {5_v, 0_v}),

                                 dasm(OPCODE_CONST_4, {1_v, 1_L}),
                                 dasm(OPCODE_MOVE, {2_v, 1_v}),
                                 dasm(OPCODE_MOVE, {4_v, 1_v}),
                                 dasm(OPCODE_MOVE_16, {0_v, 5_v}),

                                 dasm(OPCODE_MOVE, {3_v, 0_v}),
                                 dasm(OPCODE_RETURN, {3_v})};
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}

TEST_F(RegAllocTest, ParamFirstUse) {
  using namespace dex_asm;

  DexMethod* method = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "bar", "I", {"I", "I"}));
  method->make_concrete(ACC_STATIC, false);
  IRCode code(method, 0);
  EXPECT_EQ(*code.begin()->insn, *dasm(IOPCODE_LOAD_PARAM, {0_v}));
  EXPECT_EQ(*std::next(code.begin())->insn, *dasm(IOPCODE_LOAD_PARAM, {1_v}));
  code.push_back(dasm(OPCODE_CONST_4, {1_v}));
  code.push_back(dasm(OPCODE_CONST_4, {2_v}));
  code.push_back(dasm(OPCODE_ADD_INT, {3_v, 0_v, 2_v}));
  code.push_back(dasm(OPCODE_RETURN, {3_v}));
  code.set_registers_size(4);
  code.build_cfg();
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(const_cast<Block*>(cfg.exit_block()));
  fixpoint_iter.run(LivenessDomain(code.get_registers_size()));

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, true, &code, code.get_registers_size(), range_set);

  graph_coloring::SpillPlan spill_plan;
  spill_plan.param_spills = std::unordered_set<reg_t>{0, 1};
  std::unordered_set<reg_t> new_temps;
  graph_coloring::Allocator allocator;
  auto load_param = allocator.find_param_first_uses(
      spill_plan.param_spills, true, &code);
  allocator.spill_params(ig, load_param, &code, &new_temps);

  InstructionList expected_insns {
    dasm(IOPCODE_LOAD_PARAM, {4_v}),
    dasm(IOPCODE_LOAD_PARAM, {5_v}),

    // Because v1 is getting overwritten, spill move is inserted at
    // beginning of method body.
    dasm(OPCODE_MOVE_16, {1_v, 5_v}),
    dasm(OPCODE_CONST_4, {1_v}),
    dasm(OPCODE_CONST_4, {2_v}),

    // Move is inserted before first use.
    dasm(OPCODE_MOVE_16, {0_v, 4_v}),
    dasm(OPCODE_ADD_INT, {3_v, 0_v, 2_v}),
    dasm(OPCODE_RETURN, {3_v}),
  };
  EXPECT_TRUE(expected_insns.matches(InstructionIterable(code)));
}
