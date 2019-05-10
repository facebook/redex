/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "CommonSubexpressionElimination.h"
#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"

class CommonSubexpressionEliminationTest : public RedexTest {};

void test(const std::string& code_str,
          const std::string& expected_str,
          size_t expected_instructions_eliminated) {

  auto field_a = static_cast<DexField*>(DexField::make_field("LFoo;.a:I"));
  field_a->make_concrete(ACC_PUBLIC);

  auto field_v = static_cast<DexField*>(DexField::make_field("LFoo;.v:I"));
  field_v->make_concrete(ACC_PUBLIC | ACC_VOLATILE);

  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);

  code.get()->build_cfg(/* editable */ true);
  CommonSubexpressionElimination cse(code.get()->cfg());
  bool is_static = true;
  DexType* declaring_type = nullptr;
  DexTypeList* args = DexTypeList::make_type_list({});
  cse.patch(is_static, declaring_type, args);
  code.get()->clear_cfg();
  auto stats = cse.get_stats();

  EXPECT_EQ(expected_instructions_eliminated, stats.instructions_eliminated)
      << assembler::to_string(code.get()).c_str();

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()))
      << assembler::to_string(code.get()).c_str();
};

TEST_F(CommonSubexpressionEliminationTest, simple) {
  auto code_str = R"(
    (
      (const v0 0)
      (add-int v1 v0 v0)
      (add-int v2 v0 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (add-int v1 v0 v0)
      (move v3 v1)
      (add-int v2 v0 v0)
      (move v2 v3)
    )
  )";
  test(code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, pre_values) {
  // By not initializing v0, it will start out as 'top', and a pre-value will
  // be used internally to recover from that situation and still unify the
  // add-int instructions.
  auto code_str = R"(
    (
      (add-int v1 v0 v0)
      (add-int v2 v0 v0)
    )
  )";
  auto expected_str = R"(
    (
      (add-int v1 v0 v0)
      (move v3 v1)
      (add-int v2 v0 v0)
      (move v2 v3)
    )
  )";
  test(code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, many) {
  auto code_str = R"(
    (
      (const v0 0)
      (add-int v1 v0 v0)
      (add-int v2 v0 v0)
      (add-int v3 v0 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (add-int v1 v0 v0)
      (move v4 v1)
      (add-int v2 v0 v0)
      (move v2 v4)
      (add-int v3 v0 v0)
      (move v3 v4)
    )
  )";
  test(code_str, expected_str, 2);
}

TEST_F(CommonSubexpressionEliminationTest, registers_dont_matter) {
  auto code_str = R"(
    (
      (const v0 0)
      (const v1 0)
      (add-int v2 v0 v1)
      (add-int v3 v1 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (const v1 0)
      (add-int v2 v0 v1)
      (move v4 v2)
      (add-int v3 v1 v0)
      (move v3 v4)
    )
  )";
  test(code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, commutative) {
  auto code_str = R"(
    (
      (const v0 0)
      (const v1 1)
      (add-int v2 v0 v1)
      (add-int v3 v1 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (const v1 1)
      (add-int v2 v0 v1)
      (move v4 v2)
      (add-int v3 v1 v0)
      (move v3 v4)
    )
  )";
  test(code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, wide) {
  auto code_str = R"(
    (
      (const-wide v0 0)
      (add-long v2 v0 v0)
      (add-long v4 v0 v0)
    )
  )";
  auto expected_str = R"(
    (
      (const-wide v0 0)
      (add-long v2 v0 v0)
      (move-wide v6 v2)
      (add-long v4 v0 v0)
      (move-wide v4 v6)
    )
  )";
  test(code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, object) {
  auto code_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (const-string "hello")
      (move-result-pseudo-object v1)
    )
  )";
  auto expected_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (move-object v2 v0)
      (const-string "hello")
      (move-result-pseudo-object v1)
      (move-object v1 v2)
    )
  )";
  test(code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, iget) {
  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (move v3 v1)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v2 v3)
    )
  )";
  test(code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, iget_volatile) {
  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.v:I")
      (move-result-pseudo v1)
      (iget v0 "LFoo;.v:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = code_str;
  test(code_str, expected_str, 0);
}

TEST_F(CommonSubexpressionEliminationTest, affected_by_barrier) {
  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LWhat;.ever:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
    )
  )";
  auto expected_str = code_str;
  test(code_str, expected_str, 0);
}

TEST_F(CommonSubexpressionEliminationTest, recovery_after_barrier) {
  // at a barrier, the mappings have been reset, but afterwards cse kicks in as
  // expected
  auto code_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LWhat;.ever:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v3)
    )
  )";
  auto expected_str = R"(
    (
      (const v0 0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v1)
      (invoke-static () "LWhat;.ever:()V")
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v4 v2)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v3)
      (move v3 v4)
    )
  )";
  test(code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, unaffected_by_barrier) {
  auto code_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (invoke-static () "LWhat;.ever:()V")
      (const-string "hello")
      (move-result-pseudo-object v1)
    )
  )";
  auto expected_str = R"(
    (
      (const-string "hello")
      (move-result-pseudo-object v0)
      (move-object v2 v0)
      (invoke-static () "LWhat;.ever:()V")
      (const-string "hello")
      (move-result-pseudo-object v1)
      (move-object v1 v2)
    )
  )";
  test(code_str, expected_str, 1);
}

TEST_F(CommonSubexpressionEliminationTest, top_move_tracking) {
  auto code_str = R"(
    (
      (move-object v1 v0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (iget v1 "LFoo;.a:I")
      (move-result-pseudo v3)
    )
  )";
  auto expected_str = R"(
    (
      (move-object v1 v0)
      (iget v0 "LFoo;.a:I")
      (move-result-pseudo v2)
      (move v4 v2)
      (iget v1 "LFoo;.a:I")
      (move-result-pseudo v3)
      (move v3 v4)
    )
  )";
  test(code_str, expected_str, 1);
}
