/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "IRCode.h"
#include "IROpcode.h"
#include "RedexTest.h"
#include "ReduceGotos.h"
#include "Show.h"
#include "SourceBlocks.h"

class ReduceGotosTest : public RedexTest {};

void test(const std::string& code_str,
          const std::string& expected_str,
          size_t expected_replaced_gotos_with_returns,
          size_t expected_removed_trailing_moves,
          size_t expected_inverted_conditional_branches,
          size_t expected_removed_switches = 0,
          size_t expected_reduced_switches = 0,
          size_t expected_remaining_trivial_switches = 0,
          size_t expected_removed_switch_cases = 0,
          size_t expected_replaced_trivial_switches = 0) {

  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);
  code->build_cfg();
  ReduceGotosPass::Stats stats = ReduceGotosPass::process_code(code.get());
  EXPECT_EQ(expected_replaced_gotos_with_returns,
            stats.replaced_gotos_with_returns);
  EXPECT_EQ(expected_removed_trailing_moves, stats.removed_trailing_moves);
  EXPECT_EQ(expected_inverted_conditional_branches,
            stats.inverted_conditional_branches);
  EXPECT_EQ(expected_removed_switches, stats.removed_switches);
  EXPECT_EQ(expected_reduced_switches, stats.reduced_switches);
  EXPECT_EQ(expected_remaining_trivial_switches,
            stats.remaining_trivial_switches);
  EXPECT_EQ(expected_removed_switch_cases, stats.removed_switch_cases);
  EXPECT_EQ(expected_replaced_trivial_switches,
            stats.replaced_trivial_switches);
  code->clear_cfg();
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()))
      << "  " << assembler::to_s_expr(code.get()).str() << "\n---\n"
      << "  " << assembler::to_s_expr(expected.get()).str();
}

TEST_F(ReduceGotosTest, packed_switch_useless) {
  const auto* code_str = R"(
    (
      (switch v0 (:b :a))
      (:a)
      (:b)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 1, 0, 0, 2);
}

TEST_F(ReduceGotosTest, sparse_switch_useless) {
  const auto* code_str = R"(
    (
      (switch v0 (:b :a))
      (:a 0)
      (:b 1)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 1, 0, 0, 2);
}

TEST_F(ReduceGotosTest, sparse_switch_reducible) {
  const auto* code_str = R"(
    (
      (switch v0 (:a :b :c))
      (:b 1)
      (return-void)

      (:a 0)
      (:c 16)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (switch v0 (:a :c))
      (return-void)

      (:c 16)
      (:a 0)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 0, 1, 0, 1);
}

TEST_F(ReduceGotosTest, packed_switch_reducible) {
  const auto* code_str = R"(
    (
      (switch v0 (:a :b :c))
      (:a 0)
      (return-void)

      (:b 1)
      (:c 2)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (switch v0 (:b :c))
      (return-void)

      (:c 2)
      (:b 1)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 0, 1, 0, 1);
}

TEST_F(ReduceGotosTest, trivial_irreducible_remaining_switch) {
  const auto* code_str = R"(
    (
      (load-param v0)
      (load-param v1)
      (load-param v2)
      (load-param v3)
      (load-param v4)
      (load-param v5)
      (load-param v6)
      (load-param v7)
      (load-param v8)
      (load-param v9)
      (load-param v10)
      (load-param v11)
      (load-param v12)
      (load-param v13)
      (load-param v14)
      (load-param v15)
      (switch v0 (:a :b :c))
      (:a 0)
      (:b 1)
      (return-void)

      (:c 32768)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (load-param v1)
      (load-param v2)
      (load-param v3)
      (load-param v4)
      (load-param v5)
      (load-param v6)
      (load-param v7)
      (load-param v8)
      (load-param v9)
      (load-param v10)
      (load-param v11)
      (load-param v12)
      (load-param v13)
      (load-param v14)
      (load-param v15)
      (switch v0 (:c))
      (return-void)

      (:c 32768)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 0, 1, 1, 2);
}

TEST_F(ReduceGotosTest, trivial_replaced_switch_nop) {
  const auto* code_str = R"(
    (
      (switch v0 (:a :b :c))
      (:a 1)
      (:b 2)
      (return-void)

      (:c 0)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (if-eqz v0 :c)
      (return-void)

      (:c)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 0, 1, 0, 2, 1);
}

TEST_F(ReduceGotosTest, trivial_replaced_switch_rsub_lit8) {
  const auto* code_str = R"(
    (
      (load-param v0)
      (switch v0 (:a :b :c))
      (:a 0)
      (:b 1)
      (return-void)

      (:c 16)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (rsub-int/lit v0 v0 16)
      (if-eqz v0 :c)
      (return-void)

      (:c)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 0, 1, 0, 2, 1);
}

TEST_F(ReduceGotosTest, trivial_replaced_switch_rsub) {
  const auto* code_str = R"(
    (
      (load-param v0)
      (switch v0 (:a :b :c))
      (:a 0)
      (:b 1)
      (return-void)

      (:c 256)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v0)
      (rsub-int/lit v0 v0 256)
      (if-eqz v0 :c)
      (return-void)

      (:c)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 0, 1, 0, 2, 1);
}

TEST_F(ReduceGotosTest, trivial_replaced_switch_const) {
  const auto* code_str = R"(
    (
      (load-param v0)
      (switch v0 (:a :b :c))
      (:a 0)
      (:b 1)
      (return-void)

      (:c 32768)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (load-param v1)
      (const v0 32768)
      (if-eq v0 v1 :c)
      (return-void)

      (:c)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 0, 1, 0, 2, 1);
}

TEST_F(ReduceGotosTest, trivial) {
  const auto& code_str = R"(
    (
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (return-void)
    )
  )";
  test(code_str, expected_str, 0, 0, 0);
}

TEST_F(ReduceGotosTest, basic) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (const v1 0)
      (goto :end)

      (:true)
      (const v1 1)

      (:end)
      (return v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (if-eqz v0 :true)

      (const v1 0)
      (return v1)

      (:true)
      (const v1 1)
      (return v1)
    )
  )";
  test(code_str, expected_str, 1, 0, 0);
}

TEST_F(ReduceGotosTest, move) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (const v2 0)
      (move v1 v2)
      (goto :end)

      (:true)
      (const v1 1)

      (:end)
      (return v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (if-eqz v0 :true)

      (const v2 0)
      (return v2)

      (:true)
      (const v1 1)
      (return v1)
    )
  )";
  test(code_str, expected_str, 2, 1, 0);
}

TEST_F(ReduceGotosTest, involved) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (const v2 0)
      (goto :end)

      (:true)
      (if-eqz v0 :true2)

      (const v2 1)
      (goto :end2)

      (:true2)
      (const v2 2)
      (:end2)

      (:end)
      (return v2)
    )
  )";
  const auto& expected_str = R"(
    (
      (if-eqz v0 :true)

      (const v2 0)
      (return v2)

      (:true)
      (if-eqz v0 :true2)

      (const v2 1)
      (return v2)

      (:true2)
      (const v2 2)

      (:end)
      (return v2)
    )
  )";
  test(code_str, expected_str, 2, 0, 0);
}

TEST_F(ReduceGotosTest, invert) {
  const auto& code_str = R"(
    (
      (const v2 0)

      (if-eqz v0 :true)
      (:back_jump_target)

      (return v2)

      (:true)
      (const v2 1)
      (goto :back_jump_target)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v2 0)

      (if-nez v0 :true)

      (const v2 1)

      (:true)
      (return v2)
    )
  )";
  test(code_str, expected_str, 0, 0, 1);
}

TEST_F(ReduceGotosTest, move_throw) {
  const auto& code_str = R"(
    (
      (const v2 0)

      (if-eqz v0 :true)
      (goto :throw)

      (:true)
      (return v2)

      (:throw)
      (throw v2)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v2 0)

      (if-eqz v0 :true)
      (throw v2)

      (:true)
      (return v2)
    )
  )";
  test(code_str, expected_str, 0, 0, 0);
}

TEST_F(ReduceGotosTest, duplicate_throw) {
  // Note: the duplicated "(const v2 0)" is necessary to not trigger branch
  // inversion.
  const auto& code_str = R"(
    (
      (const v2 0)

      (if-eqz v0 :true)
      (const v2 0)
      (goto :throw)

      (:true)

      (if-eqz v0 :true2)
      (const v2 0)
      (goto :throw)

      (:true2)
      (return v2)

      (:throw)
      (throw v2)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v2 0)

      (if-eqz v0 :true)
      (const v2 0)
      (throw v2)

      (:true)

      (if-eqz v0 :true2)
      (const v2 0)
      (throw v2)

      (:true2)
      (return v2)
    )
  )";
  test(code_str, expected_str, 0, 0, 0);
}

TEST_F(ReduceGotosTest, no_join_throw) {
  const auto& code_str = R"(
    (
      (const v2 0)

      (if-eqz v0 :true)
      (.try_start a)
      (sget "LFoo;.b:I")
      (goto :throw)
      (.try_end a)

      (:true)
      (return v2)

      (:throw)
      (throw v2)

      (.catch (a))
      (return v2)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v2 0)

      (if-eqz v0 :true)
      (.try_start a)
      (sget "LFoo;.b:I")
      (.try_end a)
      (throw v2)

      (.catch (a))
      (return v2)

      (:true)
      (return v2)
    )
  )";
  test(code_str, expected_str, 0, 0, 0);
}

TEST_F(ReduceGotosTest, replace_return_src_blk) {
  const auto& code_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (1 1))
      (if-eqz v0 :true)

      (.src_block "LFoo;.bar:()V" 1 (0 0))
      (const v2 0)
      (move v1 v2)
      (goto :end)

      (:true)
      (.src_block "LFoo;.bar:()V" 2 (1 1))
      (const v2 1)
      
      (:end)
      (.src_block "LFoo;.bar:()V" 3 (1 1))
      (return v2)
    )
  )";
  const auto& expected_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (1 1))
      (if-eqz v0 :true)

      (.src_block "LFoo;.bar:()V" 1 (0 0))
      (const v2 0)
      (move v1 v2)
      (.src_block "LFoo;.bar:()V" 3 (0 1))
      (return v2)

      (:true)
      (.src_block "LFoo;.bar:()V" 2 (1 1))
      (const v2 1)
      (return v2)
    )
  )";
  test(code_str, expected_str, 1, 0, 0);
}

// A return block reached ONLY by gotos (no fall-through) is inlined into its
// goto-predecessors, and the now-predecessor-less original is simplified away.
//
// NOTE: both goto-preds here have only GOTO successor edges, so both take the
// `insns_to_add` path -- this case does NOT reach the block-duplication path
// that copies the SourceBlock. The `.src_block 3` below is the original return
// block surviving as a fall-through, and the second `return` deliberately
// carries no SourceBlock. See `clone_return_block_carries_src_block` for the
// duplication path.
TEST_F(ReduceGotosTest, clone_return_keeps_src_block_per_goto_pred) {
  const auto& code_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (1 1))
      (if-eqz v0 :true)

      (.src_block "LFoo;.bar:()V" 1 (1 1))
      (const v2 0)
      (goto :end)

      (:true)
      (.src_block "LFoo;.bar:()V" 2 (1 1))
      (const v2 1)
      (goto :end)

      (:end)
      (.src_block "LFoo;.bar:()V" 3 (1 1))
      (return v2)
    )
  )";
  const auto& expected_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (1 1))
      (if-eqz v0 :true)

      (.src_block "LFoo;.bar:()V" 1 (1 1))
      (const v2 0)
      (.src_block "LFoo;.bar:()V" 3 (1 1))
      (return v2)

      (:true)
      (.src_block "LFoo;.bar:()V" 2 (1 1))
      (const v2 1)
      (return v2)
    )
  )";
  test(code_str, expected_str, 1, 0, 0);
}

// The block-duplication path (`new_block`) only runs for a goto-predecessor
// that ALSO has a BRANCH or THROW successor edge; a pred whose only successor
// is the goto takes the cheaper `insns_to_add` path instead. Here the first
// goto-pred sits inside a try region, so it carries a THROW edge and the
// duplication path runs. Every resulting return block must carry a SourceBlock
// -- without the copy the duplicate comes out bare, leaving is_hot inconsistent
// with is_not_cold/maybe_hot.
TEST_F(ReduceGotosTest, clone_return_block_carries_src_block) {
  const auto& code_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (1 1))
      (const v2 0)
      (if-eqz v0 :true)

      (.src_block "LFoo;.bar:()V" 1 (1 1))
      (.try_start a)
      (sget "LFoo;.b:I")
      (goto :end)
      (.try_end a)

      (:true)
      (.src_block "LFoo;.bar:()V" 2 (1 1))
      (const v2 1)
      (goto :end)

      (:end)
      (.src_block "LFoo;.bar:()V" 3 (1 1))
      (return v2)

      (.catch (a))
      (.src_block "LFoo;.bar:()V" 4 (1 1))
      (return v2)
    )
  )";
  auto code = assembler::ircode_from_string(code_str);
  code->build_cfg();
  ReduceGotosPass::process_code(code.get());

  auto& cfg = code->cfg();
  size_t return_blocks = 0;
  for (auto* b : cfg.blocks()) {
    auto last_it = b->get_last_insn();
    if (last_it == b->end() || !opcode::is_a_return(last_it->insn->opcode())) {
      continue;
    }
    return_blocks++;
    EXPECT_NE(source_blocks::get_first_source_block(b), nullptr)
        << "return block B" << b->id() << " has no SourceBlock:\n"
        << show(cfg);
  }
  // The duplication must actually have happened: the original single return
  // block became several.
  EXPECT_GE(return_blocks, 2u) << show(cfg);
}

// With `split_source_block_counts`, a duplicated return block must carry its
// predecessor's SHARE of the original's execution count rather than a verbatim
// copy: N clones each claiming the full count would multiply the block's
// execution mass. Here the return block runs 40 times, reached from the
// try-region predecessor (30) and the plain goto predecessor (10), so the clone
// created for the try-region pred must come out at 40 * 30/40 == 30.
// The apportionment reads each predecessor's LAST source block, so a
// predecessor whose count drops inside the block contributes only the flow
// that actually leaves it. The `:true` predecessor enters at 10 and exits at
// 4, so the try-region predecessor's share is 30/(30+4) rather than 30/(30+10)
// -- clone 40 * 30/34 == 35.29, not 40 * 30/40 == 30.
//
// n.b. the second source block has to sit in a NON-try predecessor: a
// `.src_block` placed between an instruction and the `goto` inside a
// `.try_start`/`.try_end` region is dropped during assembly, which would
// silently turn this back into a single-source-block fixture.
TEST_F(ReduceGotosTest, clone_return_block_uses_predecessor_exit_count) {
  const auto& code_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (40 100))
      (const v2 0)
      (if-eqz v0 :true)

      (.src_block "LFoo;.bar:()V" 1 (30 100))
      (.try_start a)
      (sget "LFoo;.b:I")
      (goto :end)
      (.try_end a)

      (:true)
      (.src_block "LFoo;.bar:()V" 2 (10 100))
      (const v2 1)
      (.src_block "LFoo;.bar:()V" 6 (4 100))
      (goto :end)

      (:end)
      (.src_block "LFoo;.bar:()V" 3 (40 100))
      (return v2)

      (.catch (a))
      (.src_block "LFoo;.bar:()V" 4 (0 100))
      (return v2)
    )
  )";
  auto code = assembler::ircode_from_string(code_str);
  code->build_cfg();
  ReduceGotosPass::process_code(code.get(), /* for_performance */ false,
                                /* split_source_block_counts */ true);

  auto& cfg = code->cfg();
  bool found_clone = false;
  for (auto* b : cfg.blocks()) {
    auto last_it = b->get_last_insn();
    if (last_it == b->end() || !opcode::is_a_return(last_it->insn->opcode())) {
      continue;
    }
    auto* sb = source_blocks::get_first_source_block(b);
    ASSERT_NE(sb, nullptr) << show(cfg);
    if (sb->id != SourceBlock::kSyntheticId) {
      continue;
    }
    auto val = sb->get_val(0);
    ASSERT_TRUE(val.has_value()) << show(cfg);
    // 40 * 30/34. Reading the predecessor's ENTRY count would give 30.
    EXPECT_NEAR(*val, 35.2941f, 0.01f)
        << "clone did not apportion on the predecessor's exit count:\n"
        << show(cfg);
    found_clone = true;
  }
  EXPECT_TRUE(found_clone) << "no synthetic clone found:\n" << show(cfg);
}

TEST_F(ReduceGotosTest, clone_return_block_splits_src_block_counts) {
  const auto& code_str = R"(
    (
      (.src_block "LFoo;.bar:()V" 0 (40 100))
      (const v2 0)
      (if-eqz v0 :true)

      (.src_block "LFoo;.bar:()V" 1 (30 100))
      (.try_start a)
      (sget "LFoo;.b:I")
      (goto :end)
      (.try_end a)

      (:true)
      (.src_block "LFoo;.bar:()V" 2 (10 100))
      (const v2 1)
      (goto :end)

      (:end)
      (.src_block "LFoo;.bar:()V" 3 (40 100))
      (return v2)

      (.catch (a))
      (.src_block "LFoo;.bar:()V" 4 (0 100))
      (return v2)
    )
  )";
  auto code = assembler::ircode_from_string(code_str);
  code->build_cfg();
  ReduceGotosPass::process_code(code.get(), /* for_performance */ false,
                                /* split_source_block_counts */ true);

  auto& cfg = code->cfg();
  bool found_split_clone = false;
  for (auto* b : cfg.blocks()) {
    auto last_it = b->get_last_insn();
    if (last_it == b->end() || !opcode::is_a_return(last_it->insn->opcode())) {
      continue;
    }
    auto* sb = source_blocks::get_first_source_block(b);
    ASSERT_NE(sb, nullptr) << "return block B" << b->id()
                           << " has no SourceBlock:\n"
                           << show(cfg);
    auto val = sb->get_val(0);
    ASSERT_TRUE(val.has_value()) << show(cfg);
    // No return block may still claim the undivided 40 -- that is the
    // mass-duplication bug this option fixes.
    EXPECT_LT(*val, 40.0f) << "return block B" << b->id()
                           << " kept the undivided count:\n"
                           << show(cfg);
    if (*val > 29.9f && *val < 30.1f) {
      found_split_clone = true;
    }
  }
  EXPECT_TRUE(found_split_clone)
      << "expected a clone carrying 40 * 30/40 == 30:\n"
      << show(cfg);
}
