/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "UpCodeMotion.h"

class UpCodeMotionTest : public RedexTest {};

void test(const std::string& code_str,
          const std::string& expected_str,
          size_t expected_instructions_moved,
          size_t expected_branches_moved_over,
          size_t expected_inverted_conditional_branches,
          size_t expected_clobbered_registers,
          bool branch_hotness_check = true) {
  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);

  bool is_static = true;
  DexTypeList* args = DexTypeList::make_type_list({});
  DexType* declaring_type = nullptr;
  UpCodeMotionPass::Stats stats = UpCodeMotionPass::process_code(
      is_static, declaring_type, args, code.get(), branch_hotness_check);
  EXPECT_EQ(expected_instructions_moved, stats.instructions_moved);
  EXPECT_EQ(expected_branches_moved_over, stats.branches_moved_over);
  EXPECT_EQ(expected_inverted_conditional_branches,
            stats.inverted_conditional_branches);
  EXPECT_EQ(expected_clobbered_registers, stats.clobbered_registers);

  printf("%s\n", assembler::to_string(code.get()).c_str());
  EXPECT_CODE_EQ(code.get(), expected.get());
};

TEST_F(UpCodeMotionTest, basic) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (const v1 0)

      (:end)
      (return v1)

      (:true)
      (const v1 1)
      (goto :end)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 1)
      (if-eqz v0 :end)

      (const v1 0)

      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str, 1, 1, 0, 0);
}

TEST_F(UpCodeMotionTest, move) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (const v1 0)

      (:end)
      (return v1)

      (:true)
      (move v1 v2)
      (goto :end)
    )
  )";
  const auto& expected_str = R"(
    (
      (move v1 v2)
      (if-eqz v0 :end)

      (const v1 0)

      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str, 1, 1, 0, 0);
}

TEST_F(UpCodeMotionTest, add_ints) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (add-int v1 v3 v4)
      (add-int v2 v5 v6)

      (:end)
      (return v1)

      (:true)
      (const v1 0)
      (const v2 0)
      (goto :end)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 0)
      (const v2 0)
      (if-eqz v0 :end)

      (add-int v1 v3 v4)
      (add-int v2 v5 v6)

      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str, 2, 1, 0, 0);
}

TEST_F(UpCodeMotionTest, goto_source_overlaps_with_branch_dest) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (add-int v1 v2 v3)
      (const v2 0)

      (:end)
      (return v1)

      (:true)
      (xor-int v1 v2 v3)
      (const v2 0)
      (goto :end)
    )
  )";
  const auto& expected_str = code_str;
  test(code_str, expected_str, 0, 0, 0, 0);
}

TEST_F(UpCodeMotionTest, multiple_consts) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (const v1 0)
      (const v2 0)

      (:end)
      (return v1)

      (:true)
      (const v1 1)
      (const v2 1)
      (goto :end)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 1)
      (const v2 1)
      (if-eqz v0 :end)

      (const v1 0)
      (const v2 0)

      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str, 2, 1, 0, 0);
}

TEST_F(UpCodeMotionTest, basic_invert) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (const v1 0)

      (:end)
      (return v1)

      (:true)
      (const v1 1)
      (const-string "hello")
      (move-result-pseudo v2)
      (goto :end)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v1 0)
      (if-nez v0 :end)

      (const v1 1)
      (const-string "hello")
      (move-result-pseudo v2)

      (:end)
      (return v1)
    )
  )";
  test(code_str, expected_str, 1, 1, 1, 0);
}

TEST_F(UpCodeMotionTest, no_const_wides) {
  const auto& code_str = R"(
    (
      (if-eqz v0 :true)

      (const-wide v1 0)

      (:end)
      (return v1)

      (:true)
      (const-wide v1 1)
      (goto :end)
    )
  )";
  const auto& expected_str = R"(
    (
      (if-eqz v0 :true)

      (const-wide v1 0)

      (:end)
      (return v1)

      (:true)
      (const-wide v1 1)
      (goto :end)
    )
  )";
  test(code_str, expected_str, 0, 0, 0, 0);
}

TEST_F(UpCodeMotionTest, clobbered_scalar) {
  const auto& code_str = R"(
    (
      (const v0 0)
      (if-eqz v0 :true)

      (const v0 0)

      (:end)
      (return v0)

      (:true)
      (const v0 1)
      (goto :end)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 0)
      (move v1 v0)
      (const v0 1)
      (if-eqz v1 :end)

      (const v0 0)

      (:end)
      (return v0)
    )
  )";
  test(code_str, expected_str, 1, 1, 0, 1);
}

TEST_F(UpCodeMotionTest, clobbered_reference) {
  const auto& code_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (if-eqz v0 :true)

      (const v0 0)

      (:end)
      (return v0)

      (:true)
      (const v0 1)
      (goto :end)
    )
  )";
  const auto& expected_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (move-object v1 v0)
      (const v0 1)
      (if-eqz v1 :end)

      (const v0 0)

      (:end)
      (return v0)
    )
  )";
  test(code_str, expected_str, 1, 1, 0, 1);
}

TEST_F(UpCodeMotionTest, clobbered_two_references) {
  const auto& code_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (const-string "hello2")
      (move-result-pseudo-object v1)
      (if-eq v0 v1 :true)

      (const v0 0)
      (const v1 0)

      (:end)
      (return v0)

      (:true)
      (const v0 1)
      (const v1 1)
      (goto :end)
    )
  )";
  const auto& expected_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (const-string "hello2")
      (move-result-pseudo-object v1)
      (move-object v2 v0)
      (move-object v3 v1)
      (const v0 1)
      (const v1 1)
      (if-eq v2 v3 :end)

      (const v0 0)
      (const v1 0)

      (:end)
      (return v0)
    )
  )";
  test(code_str, expected_str, 2, 1, 0, 2);
}

/*
 * Check if instructions are moved if the branching block is not hot
 * (should move)
 * */
TEST_F(UpCodeMotionTest, hot_branch) {
  const auto& code_str = R"(
  (
    (if-eqz v0 :L1)

    (const v1 0)
    (const v2 0)

    (:L0)
    (return v1)

    (:L1)
    (.src_block "LFoo;.m:()V" 2 ())
    (const v2 1)
    (goto :L0))

  )";

  const auto& expected_str = R"(
  (
    (const v2 1)
    (if-eqz v0 :L0)

    (const v1 0)
    (const v2 0)

    (:L0)
    (return v1)

    )
)";

  test(code_str, expected_str, 1, 1, 0, 0);
}

TEST_F(UpCodeMotionTest, hot_branch_2) {
  const auto& code_str = R"(
  (
      (.src_block "LFoo;.m:()V" 1 (0.1 0.2))
      (if-eqz v0 :true)

      (const v1 0)
      (const v2 0)
      (:end)
      (return v1)

      (:true)
      (.src_block "LFoo;.k:()V" 2 (0.0 0.0))
      (const v2 1)
      (goto :end))

  )";

  const auto& expected_str = R"(
  (
      (.src_block "LFoo;.m:()V" 1 (0.1 0.2))
      (if-eqz v0 :true)

      (const v1 0)
      (const v2 0)
      (:end)
      (return v1)

      (:true)
      (.src_block "LFoo;.k:()V" 2 (0.0 0.0))
      (const v2 1)
      (goto :end)
    )
)";

  test(code_str, expected_str, 0, 0, 0, 0);
}
