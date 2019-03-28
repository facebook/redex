/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "BranchPrefixHoisting.h"
#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"

class BranchPrefixHoistingTest : public RedexTest {};

void test(const std::string& code_str,
          const std::string& expected_str,
          size_t expected_instructions_hoisted) {
  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);
  auto code_ptr = code.get();
  code_ptr->build_cfg(true);
  auto& cfg = code_ptr->cfg();
  std::cerr << "before:" << std::endl << SHOW(cfg);

  int actual_insns_hoisted = BranchPrefixHoistingPass::process_cfg(cfg);

  std::cerr << "after:" << std::endl << SHOW(cfg);

  auto expected_ptr = expected.get();
  expected_ptr->build_cfg(true);
  auto& expected_cfg = expected_ptr->cfg();
  std::cerr << "expected:" << std::endl << SHOW(expected_cfg);

  code_ptr->clear_cfg();
  expected_ptr->clear_cfg();

  EXPECT_EQ(expected_instructions_hoisted, actual_insns_hoisted);
  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()));
}

TEST_F(BranchPrefixHoistingTest, simple_insn_hoisting) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (const v6 6)
      (goto :end)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (const v6 7)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (if-eqz v0 :true)
      (const v6 6)
      (goto :end)
      (:true)
      (const v6 7)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 5);
}

TEST_F(BranchPrefixHoistingTest, stop_hoisting_at_side_effect) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v0 7)
      (goto :end)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v0 7)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (if-eqz v0 :true)
      (const v0 7)
      (goto :end)
      (:true)
      (const v0 7)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 3);
}

TEST_F(BranchPrefixHoistingTest, move_result_hoist_ok) {
  const auto& code_str = R"(
    (
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 16)
      (const v2 8)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (if-eqz v0 :true)
      (const v5 42)
      (goto :end)
      (:true)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 2);
}

TEST_F(BranchPrefixHoistingTest, move_result_no_hoist_diff_dest) {
  const auto& code_str = R"(
    (
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v4)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v4)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v3)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0);
}

TEST_F(BranchPrefixHoistingTest, move_result_no_hoist_on_side_effect) {
  const auto& code_str = R"(
    (
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v0)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v0)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 16)
      (const v2 8)
      (if-eqz v0 :true)
      (div-int v1 v2)
      (move-result-pseudo v0)
      (const v5 42)
      (goto :end)
      (:true)
      (div-int v1 v2)
      (move-result-pseudo v0)
      (const v6 43)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0);
}

TEST_F(BranchPrefixHoistingTest, one_block_becomes_empty) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)
      (const v1 1)
      (const v2 2)
      (goto :end)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 1)
      (const v2 2)
      (if-eqz v0 :true)
      (goto :end)
      (:true)
      (const v3 3)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 2);
}

TEST_F(BranchPrefixHoistingTest, both_blocks_becomes_empty) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (goto :end)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (if-eqz v0 :true)
      (goto :end)
      (:true)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 3);
}

TEST_F(BranchPrefixHoistingTest, move_result_wide) {
  const auto& code_str = R"(
    (
      (const-wide v1 2)
      (const-wide v2 10)
      (if-ge v3 v0 :true)
      (invoke-static (v1 v2) "LCls;.max:(JJ)J")
      (move-result-wide v0)
      (goto :end)
      (:true)
      (invoke-static (v1 v2) "LCls;.max:(JJ)J")
      (move-result-wide v0)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const-wide v1 2)
      (const-wide v2 10)
      (if-ge v3 v0 :true)
      (invoke-static (v1 v2) "LCls;.max:(JJ)J")
      (move-result-wide v0)
      (goto :end)
      (:true)
      (invoke-static (v1 v2) "LCls;.max:(JJ)J")
      (move-result-wide v0)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0);
}

TEST_F(BranchPrefixHoistingTest, branch_goes_to_same_block) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (const v6 7)
      (:end)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (if-eqz v0 :true)
      (:true)
      (const v1 1)
      (const v2 2)
      (const v3 3)
      (const v4 4)
      (const v5 5)
      (const v6 7)
      (:end)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0);
}

TEST_F(BranchPrefixHoistingTest, switch_two_same_cases) {
  const auto& code_str = R"(
    (
      (const v0 0)
      (sparse-switch v0 (:case1 :case1))
      (:case1 1)
      (const v1 1)
      (const v2 2)
      (return-void)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 0)
      (sparse-switch v0 (:case1 :case1))
      (:case1 1)
      (const v1 1)
      (const v2 2)
      (return-void)
    )
  )";
  test(code_str, expected_str, 0);
}
