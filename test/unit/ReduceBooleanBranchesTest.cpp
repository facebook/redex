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
#include "ReduceBooleanBranches.h"

class ReduceBooleanBranchesTest : public RedexTest {};

void test(const std::string& code_str,
          const std::string& expected_str,
          size_t expected_boolean_branches_removed,
          size_t expected_object_branches_removed,
          size_t expected_xors_reduced) {
  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);

  code->build_cfg();
  reduce_boolean_branches_impl::ReduceBooleanBranches rbb(
      {}, /* is_static */ true, /* args */ nullptr, code.get());
  rbb.run();
  auto stats = rbb.get_stats();
  code->clear_cfg();
  EXPECT_EQ(expected_boolean_branches_removed, stats.boolean_branches_removed);
  EXPECT_EQ(expected_object_branches_removed, stats.object_branches_removed);
  EXPECT_EQ(expected_xors_reduced, stats.xors_reduced);

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()))
      << "  " << assembler::to_s_expr(code.get()).str() << "\n---\n"
      << "  " << assembler::to_s_expr(expected.get()).str();
}

TEST_F(ReduceBooleanBranchesTest, boolean_negation_diamond) {
  auto code_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (if-eqz v0 :a)
      (const v0 0)
      (goto :b)
      (:a)
      (const v0 1)
      (:b)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (xor-int/lit v0 v0 1)
      (return v0)
    )
  )";
  test(code_str, expected_str, 1, 0, 0);
}

TEST_F(ReduceBooleanBranchesTest, boolean_redundant_diamond) {
  auto code_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (if-nez v0 :a)
      (const v0 0)
      (goto :b)
      (:a)
      (const v0 1)
      (:b)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (move v0 v0)
      (return v0)
    )
  )";
  test(code_str, expected_str, 1, 0, 0);
}

TEST_F(ReduceBooleanBranchesTest, boolean_redundant_diamond2) {
  auto code_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (if-eqz v0 :a)
      (const v0 1)
      (goto :b)
      (:a)
      (const v0 0)
      (:b)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (move v0 v0)
      (return v0)
    )
  )";
  test(code_str, expected_str, 1, 0, 0);
}

TEST_F(ReduceBooleanBranchesTest, object_positive_diamond) {
  auto code_str = R"(
    (
      (sget-object "LFoo;.bar:LBar;")
      (move-result-pseudo-object v0)
      (if-nez v0 :a)
      (const v0 0)
      (goto :b)
      (:a)
      (const v0 1)
      (:b)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (sget-object "LFoo;.bar:LBar;")
      (move-result-pseudo-object v0)
      (instance-of v0 "Ljava/lang/Object;")
      (move-result-pseudo v0)
      (return v0)
    )
  )";
  test(code_str, expected_str, 0, 1, 0);
}

TEST_F(ReduceBooleanBranchesTest, object_negative_diamond) {
  auto code_str = R"(
    (
      (sget-object "LFoo;.bar:LBar;")
      (move-result-pseudo-object v0)
      (if-eqz v0 :a)
      (const v0 0)
      (goto :b)
      (:a)
      (const v0 1)
      (:b)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (sget-object "LFoo;.bar:LBar;")
      (move-result-pseudo-object v0)
      (instance-of v0 "Ljava/lang/Object;")
      (move-result-pseudo v0)
      (xor-int/lit v0 v0 1)
      (return v0)
    )
  )";
  test(code_str, expected_str, 0, 1, 0);
}

TEST_F(ReduceBooleanBranchesTest, reduce_xor_conditional_branch) {
  auto code_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (xor-int/lit v0 v0 1)
      (if-eqz v0 :a)
      (const v0 42)
      (return v0)
      (:a)
      (const v0 23)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (move v1 v0)
      (xor-int/lit v0 v0 1)
      (if-nez v1 :a)
      (const v0 42)
      (return v0)
      (:a)
      (const v0 23)
      (return v0)
    )
  )";
  test(code_str, expected_str, 0, 0, 1);
}

TEST_F(ReduceBooleanBranchesTest, reduce_xor_xor) {
  auto code_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (xor-int/lit v0 v0 1)
      (xor-int/lit v0 v0 1)
      (return v0)
    )
  )";
  const auto& expected_str = R"(
    (
      (sget-boolean "LFoo;.bar:Z")
      (move-result-pseudo v0)
      (move v1 v0)
      (xor-int/lit v0 v0 1)
      (move v0 v1)
      (return v0)
    )
  )";
  test(code_str, expected_str, 0, 0, 1);
}
