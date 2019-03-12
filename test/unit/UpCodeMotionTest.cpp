/**
 * Copyright (c) Facebook, Inc. and its affiliates.
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
          size_t expected_inverted_conditional_branches) {
  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);

  UpCodeMotionPass::Stats stats = UpCodeMotionPass::process_code(code.get());
  EXPECT_EQ(expected_instructions_moved, stats.instructions_moved);
  EXPECT_EQ(expected_branches_moved_over, stats.branches_moved_over);
  EXPECT_EQ(expected_inverted_conditional_branches,
            stats.inverted_conditional_branches);

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()));
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
  test(code_str, expected_str, 1, 1, 0);
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
  test(code_str, expected_str, 2, 1, 0);
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
  test(code_str, expected_str, 1, 1, 1);
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
  test(code_str, expected_str, 0, 0, 0);
}
