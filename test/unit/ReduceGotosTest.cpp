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
#include "ReduceGotos.h"

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

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()))
      << "  " << assembler::to_s_expr(code.get()).str() << "\n---\n"
      << "  " << assembler::to_s_expr(expected.get()).str();
}

TEST_F(ReduceGotosTest, packed_switch_useless) {
  auto code_str = R"(
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
  auto code_str = R"(
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
  auto code_str = R"(
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
  auto code_str = R"(
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
  auto code_str = R"(
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
  auto code_str = R"(
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
  auto code_str = R"(
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
  auto code_str = R"(
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
  auto code_str = R"(
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
