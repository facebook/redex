/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cmath>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexAsm.h"
#include "DexUtil.h"
#include "GraphColoring.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "Interference.h"
#include "LiveRange.h"
#include "Liveness.h"
#include "OpcodeList.h"
#include "RedexTest.h"
#include "RegAlloc.h"
#include "RegisterType.h"
#include "Show.h"
#include "Transform.h"
#include "Util.h"
#include "VirtualRegistersFile.h"

using namespace regalloc;

struct RegAllocTest : public RedexTest {};

/*
 * Check that we pick the most pessimistic move instruction (of the right type)
 * that can address arbitrarily large registers -- we will shrink it down later
 * as necessary when syncing the IRCode.
 */
TEST_F(RegAllocTest, MoveGen) {
  using namespace dex_asm;
  EXPECT_EQ(*gen_move(RegisterType::NORMAL, 1, 2),
            *dasm(OPCODE_MOVE, {1_v, 2_v}));
  EXPECT_EQ(*gen_move(RegisterType::ZERO, 1, 2),
            *dasm(OPCODE_MOVE, {1_v, 2_v}));
  EXPECT_EQ(*gen_move(RegisterType::OBJECT, 1, 2),
            *dasm(OPCODE_MOVE_OBJECT, {1_v, 2_v}));
  EXPECT_EQ(*gen_move(RegisterType::WIDE, 1, 2),
            *dasm(OPCODE_MOVE_WIDE, {1_v, 2_v}));
}

TEST_F(RegAllocTest, RegTypeDestWide) {
  // check for consistency...
  for (auto op : all_opcodes) {
    // We cannot create IRInstructions from these opcodes
    auto insn = std::make_unique<IRInstruction>(op);
    if (insn->dests_size()) {
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
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (return-void)
    )
)");
  code->set_registers_size(1);

  live_range::renumber_registers(code.get(), /* width_aware */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v1)
     (check-cast v1 "Ljava/lang/Object;")
     (move-result-pseudo-object v2)
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
  EXPECT_EQ(code->get_registers_size(), 3);
}

TEST_F(RegAllocTest, LiveRange) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (if-eq v0 v0 :if-true-label)

     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)

     (:if-true-label)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (return-void)
    )
)");

  live_range::renumber_registers(code.get(), /* width_aware */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v1)
     (check-cast v1 "Ljava/lang/Object;")
     (move-result-pseudo-object v2)
     (if-eq v2 v2 :if-true-label)

     (const v3 0)
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v4)
     (check-cast v4 "Ljava/lang/Object;")
     (move-result-pseudo-object v2)

     (:if-true-label)
     (check-cast v2 "Ljava/lang/Object;")
     (move-result-pseudo-object v5)
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_string(code.get()),
            assembler::to_string(expected_code.get()));
  EXPECT_EQ(code->get_registers_size(), 6);
}

TEST_F(RegAllocTest, WidthAwareLiveRange) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const-wide v0 0)
     (sput-wide v0 "LFoo;.bar:I")
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (check-cast v0 "Ljava/lang/Object;")
     (move-result-pseudo-object v0)
     (return-void)
    )
)");

  live_range::renumber_registers(code.get(), /* width_aware */ true);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const-wide v1 0)
     (sput-wide v1 "LFoo;.bar:I")
     (new-instance "Ljava/lang/Object;")
     (move-result-pseudo-object v3) ; skip v2 since we have a wide value in v1
     (check-cast v3 "Ljava/lang/Object;")
     (move-result-pseudo-object v4)
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
  EXPECT_EQ(code->get_registers_size(), 5);
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
  EXPECT_EQ(fp_div_ceil(1, 1), edge_weight_helper(1, 1));
  EXPECT_EQ(fp_div_ceil(1, 2), edge_weight_helper(2, 1));
  EXPECT_EQ(fp_div_ceil(2, 1), edge_weight_helper(1, 2));
  EXPECT_EQ(fp_div_ceil(2, 2), edge_weight_helper(2, 2));
}

TEST_F(RegAllocTest, BuildInterferenceGraph) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     (const v2 0)
     (add-int v3 v0 v2)
     (return v3)
    )
)");
  code->set_registers_size(4);

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);
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
  EXPECT_EQ(ig.get_node(2).max_vreg(), 255);
  EXPECT_EQ(ig.get_node(2).adjacent(), std::vector<reg_t>{0});
  EXPECT_EQ(ig.get_node(2).type(), RegisterType::NORMAL);
  EXPECT_EQ(ig.get_node(2).spill_cost(), 2);
  EXPECT_EQ(ig.get_node(3).max_vreg(), 255);
  EXPECT_EQ(ig.get_node(3).adjacent(), std::vector<reg_t>{});
  EXPECT_EQ(ig.get_node(3).type(), RegisterType::NORMAL);
  EXPECT_EQ(ig.get_node(3).spill_cost(), 2);

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
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (move v1 v0)
     (return v1)
    )
)");
  code->set_registers_size(2);

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);
  graph_coloring::Allocator allocator;
  allocator.coalesce(&ig, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     ; move opcode was coalesced
     (return v0)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(RegAllocTest, MoveWideCoalesce) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const-wide v0 0)
     (move-wide v1 v0)
     (return-wide v1)
    )
)");
  code->set_registers_size(2);
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);

  EXPECT_TRUE(ig.is_coalesceable(0, 1));
  EXPECT_TRUE(ig.is_adjacent(0, 1));

  graph_coloring::Allocator allocator;
  allocator.coalesce(&ig, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const-wide v0 0)
     ; move-wide opcode was coalesced
     (return-wide v0)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(RegAllocTest, NoCoalesceWide) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const-wide v0 0)
     (move-wide v1 v0) ; This move can't be coalesced away due to the
                       ; long-to-double instruction below
     (long-to-double v1 v0)
     (return-wide v0)
    )
)");
  code->set_registers_size(2);
  auto original_code_s_expr = assembler::to_s_expr(code.get());

  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);

  EXPECT_FALSE(ig.is_coalesceable(0, 1));
  EXPECT_TRUE(ig.is_adjacent(0, 1));

  graph_coloring::Allocator allocator;
  allocator.coalesce(&ig, code.get());

  EXPECT_EQ(assembler::to_s_expr(code.get()), original_code_s_expr);
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
  allocator.simplify(&ig, &select_stack, &spilled_select_stack);
  auto selected = stack_to_vec(select_stack);
  EXPECT_EQ(selected, std::vector<reg_t>({1, 0, 2}));
}

TEST_F(RegAllocTest, SelectRange) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     (load-param v2)
     (load-param v3)
     (load-param v4)
     (load-param v5)

     ; the invoke instruction references the param registers in order; make
     ; sure we map them 1:1 without any spills, and map v6 to the start of the
     ; frame (since the params must be at the end)
     (const v6 0)
     (invoke-static (v0 v1 v2 v3 v4 v5) "Lfoo;.baz:(IIIIII)V")

     (add-int v3 v0 v6)
     (return v3)
    )
)");
  code->set_registers_size(7);
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set = init_range_set(code.get());
  EXPECT_EQ(range_set.size(), 1);
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);
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
  allocator.simplify(&ig, &select_stack, &spilled_select_stack);
  allocator.select(code.get(), ig, &select_stack, &reg_transform, &spill_plan);
  // v3 is referenced by both range and non-range instructions. We should not
  // allocate it in select() but leave it to select_ranges()
  EXPECT_EQ(reg_transform.map, (transform::RegMap{{6, 0}}));
  allocator.select_ranges(
      code.get(), ig, range_set, &reg_transform, &spill_plan);
  EXPECT_EQ(reg_transform.map,
            (transform::RegMap{
                {0, 1}, {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 0}}));
  EXPECT_EQ(reg_transform.size, 7);
  EXPECT_TRUE(spill_plan.empty());
}

TEST_F(RegAllocTest, SelectAliasedRange) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (invoke-static (v0 v0) "Lfoo;.baz:(II)V")
     (return-void)
    )
)");
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  auto invoke_it =
      std::find_if(code->begin(), code->end(), [](const MethodItemEntry& mie) {
        return mie.type == MFLOW_OPCODE &&
               mie.insn->opcode() == OPCODE_INVOKE_STATIC;
      });
  ASSERT_NE(invoke_it, code->end());
  auto invoke = invoke_it->insn;
  RangeSet range_set;
  range_set.emplace(invoke);
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);
  graph_coloring::SpillPlan spill_plan;
  graph_coloring::RegisterTransform reg_transform;
  graph_coloring::Allocator allocator;
  allocator.select_ranges(
      code.get(), ig, range_set, &reg_transform, &spill_plan);

  EXPECT_EQ(spill_plan.range_spills.at(invoke), std::vector<size_t>{1});

  allocator.spill(ig, spill_plan, range_set, code.get());
  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (move v1 v0)
     (invoke-static (v0 v1) "Lfoo;.baz:(II)V")
     (return-void)
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

/*
 * If two ranges use the same symregs in the same order, we should try and map
 * them to the same vregs.
 */
TEST_F(RegAllocTest, AlignRanges) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (const v1 1)
     (invoke-static (v0 v1) "Lfoo;.baz:(II)V")
     (invoke-static (v0 v1) "Lfoo;.baz:(II)V")
     (return-void)
    )
)");
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  for (auto& mie : InstructionIterable(code.get())) {
    if (mie.insn->opcode() == OPCODE_INVOKE_STATIC) {
      range_set.emplace(mie.insn);
    }
  }
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);
  graph_coloring::SpillPlan spill_plan;
  graph_coloring::RegisterTransform reg_transform;
  graph_coloring::Allocator allocator;
  allocator.select_ranges(
      code.get(), ig, range_set, &reg_transform, &spill_plan);

  EXPECT_EQ(reg_transform.map, (transform::RegMap{{0, 0}, {1, 1}}));
  EXPECT_EQ(reg_transform.size, 2);
  EXPECT_TRUE(spill_plan.range_spills.empty());
}

TEST_F(RegAllocTest, Spill) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param-object v3)
     (iget v3 "LFoo;.a:I")
     (move-result-pseudo v0)
     (iget v3 "LFoo;.b:I")
     (move-result-pseudo v1)
     (add-int v2 v0 v1)
     (return v2)
    )
)");
  code->set_registers_size(4);
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);

  SplitPlan split_plan;
  graph_coloring::SpillPlan spill_plan;
  spill_plan.global_spills = std::unordered_map<reg_t, reg_t> {
    {0, 16},
    {1, 16},
    {2, 256},
  };
  graph_coloring::Allocator allocator;
  allocator.spill(ig, spill_plan, range_set, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param-object v3)
     (iget v3 "LFoo;.a:I")
     (move-result-pseudo v4)
     (move v0 v4)
     (iget v3 "LFoo;.b:I")
     (move-result-pseudo v5)
     (move v1 v5)

     (add-int v6 v0 v1) ; srcs not spilled -- add-int can address up to
                        ; 8-bit-sized operands
     (move v2 v6)

     (move v7 v2)
     (return v7)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(RegAllocTest, NoSpillSingleArgInvokes) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (neg-int v1 v0) ; neg-int's operands are limited to 4 bits
     (invoke-static (v0) "Lfoo;.baz:(I)V") ; this can always be converted to
                                           ; an invoke-range, so it should not
                                           ; get spilled
     (return-void)
    )
)");
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);

  SplitPlan split_plan;
  graph_coloring::SpillPlan spill_plan;
  spill_plan.global_spills = std::unordered_map<reg_t, reg_t> {
    {0, 16},
    {1, 0},
  };
  graph_coloring::Allocator allocator;
  allocator.spill(ig, spill_plan, range_set, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 0)
     (move v2 v0)
     (neg-int v1 v2)
     (invoke-static (v0) "Lfoo;.baz:(I)V")
     (return-void)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(RegAllocTest, ContainmentGraph) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     (move v2 v0)
     (move v3 v1)
     (add-int v4 v2 v3)
     (return v4)
    )
)");

  code->set_registers_size(5);
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);
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
  allocator.coalesce(&ig, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     ; move opcodes were coalesced
     (add-int v0 v0 v1)
     (return v0)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
  EXPECT_TRUE(ig.has_containment_edge(1, 0));
  EXPECT_TRUE(ig.has_containment_edge(0, 1));
}

TEST_F(RegAllocTest, FindSplit) {
  auto code = assembler::ircode_from_string(R"(
    (
     (sget "LFoo.a:I")
     (move-result-pseudo v0)
     (sget "LFoo.a:I")
     (move-result-pseudo v1)
     (sput v1 "LFoo.a:I")
     (sput v1 "LFoo.a:I")
     (return v0)
    )
)");
  code->set_registers_size(5);
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);

  SplitCosts split_costs;
  SplitPlan split_plan;
  graph_coloring::SpillPlan spill_plan;
  spill_plan.global_spills = std::unordered_map<reg_t, reg_t>{{1, 256}};
  graph_coloring::RegisterTransform reg_transform;
  reg_transform.map = transform::RegMap{{0, 0}};
  graph_coloring::Allocator allocator;
  calc_split_costs(fixpoint_iter, code.get(), &split_costs);
  allocator.find_split(
      ig, split_costs, &reg_transform, &spill_plan, &split_plan);
  EXPECT_EQ(split_plan.split_around.at(1), std::unordered_set<reg_t>{0});
}

TEST_F(RegAllocTest, Split) {
  auto code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (const v1 1)
     (move v2 v1)
     (move v4 v1)
     (move v3 v0)
     (return v3)
    )
)");
  code->set_registers_size(5);
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);

  SplitCosts split_costs;
  SplitPlan split_plan;
  graph_coloring::SpillPlan spill_plan;
  // split 0 around 1
  split_plan.split_around =
      std::unordered_map<reg_t, std::unordered_set<reg_t>>{
          {1, std::unordered_set<reg_t>{0}}};
  graph_coloring::Allocator allocator;
  allocator.spill(ig, spill_plan, range_set, code.get());
  split(fixpoint_iter, split_plan, split_costs, ig, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 1)
     (move v5 v0)

     (const v1 1)
     (move v2 v1)
     (move v4 v1)
     (move v0 v5)

     (move v3 v0)
     (return v3)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(RegAllocTest, ParamFirstUse) {
  auto code = assembler::ircode_from_string(R"(
    (
     (load-param v0)
     (load-param v1)
     (const v1 0)
     (const v2 0)
     (add-int v3 v0 v2)
     (return v3)
    )
)");
  code->set_registers_size(4);
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());

  RangeSet range_set;
  interference::Graph ig = interference::build_graph(
      fixpoint_iter, code.get(), code->get_registers_size(), range_set);

  graph_coloring::SpillPlan spill_plan;
  spill_plan.param_spills = std::unordered_set<reg_t>{0, 1};
  graph_coloring::Allocator allocator;
  allocator.split_params(ig, spill_plan.param_spills, code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param v4)
     (load-param v5)

     ; Since v1 was getting overwritten in the original code, we insert a load
     ; immediately after the load-param instructions
     (move v1 v5)
     (const v1 0)
     (const v2 0)

     ; Since v0 did not get overwritten in the original code, we are able to
     ; insert the load before its first use
     (move v0 v4)
     (add-int v3 v0 v2)
     (return v3)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

TEST_F(RegAllocTest, NoOverwriteThis) {
  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:(I)LFoo;"));
  method->make_concrete(ACC_PUBLIC, /* is_virtual */ true);

  method->set_code(assembler::ircode_from_string(R"(
    (
     (load-param-object v0)
     (load-param v1)
     (if-eqz v1 :true-label)
     (sget-object "LFoo;.foo:LFoo;")
     (move-result-object v0)
     (:true-label)
     (return-object v0)
    )
)"));
  auto code = method->get_code();
  code->set_registers_size(2);
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  graph_coloring::Allocator::Config config;
  config.no_overwrite_this = true;
  graph_coloring::Allocator allocator(config);
  allocator.allocate(method);

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (load-param-object v1)
     (load-param v2)
     (move-object v0 v1)
     (if-eqz v2 :true-label)
     (sget-object "LFoo;.foo:LFoo;")
     (move-result-object v0)
     (:true-label)
     (return-object v0)
    )
)");
  EXPECT_EQ(assembler::to_s_expr(code),
            assembler::to_s_expr(expected_code.get()))
      << show(code);
}
