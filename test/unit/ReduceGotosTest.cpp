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
#include "ReduceGotos.h"

class ReduceGotosTest : public RedexTest {};

void test(const std::string& code_str,
          const std::string& expected_str,
          size_t expected_replaced_gotos_with_returns,
          size_t expected_removed_trailing_moves,
          size_t expected_inverted_conditional_branches) {
  g_redex = new RedexContext();

  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);

  ReduceGotosPass::Stats stats = ReduceGotosPass::process_code(code.get());
  EXPECT_EQ(expected_replaced_gotos_with_returns,
            stats.replaced_gotos_with_returns);
  EXPECT_EQ(expected_removed_trailing_moves, stats.removed_trailing_moves);
  EXPECT_EQ(expected_inverted_conditional_branches,
            stats.inverted_conditional_branches);

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()));

  delete g_redex;
};

TEST(ReduceGotosTest, trivial) {
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

TEST(ReduceGotosTest, basic) {
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

TEST(ReduceGotosTest, move) {
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

TEST(ReduceGotosTest, involved) {
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

TEST(ReduceGotosTest, invert) {
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
