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
#include "ReduceArrayLiterals.h"

class ReduceArrayLiteralsTest : public RedexTest {};

void test(const std::string& code_str,
          const std::string& expected_str,
          size_t expected_filled_arrays,
          size_t expected_filled_array_elements,
          size_t max_filled_elements = 222,
          int32_t min_sdk = 24,
          Architecture arch = Architecture::UNKNOWN) {
  auto code = assembler::ircode_from_string(code_str);
  auto expected = assembler::ircode_from_string(expected_str);

  code->build_cfg();
  ReduceArrayLiterals ral(code->cfg(), max_filled_elements, min_sdk, arch);
  ral.patch();
  code->clear_cfg();
  auto stats = ral.get_stats();

  // printf("ACTUAL CODE: \n %s\n", assembler::to_string(code.get()).c_str());

  EXPECT_EQ(expected_filled_arrays, stats.filled_arrays);
  EXPECT_EQ(expected_filled_array_elements, stats.filled_array_elements);

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected.get()));
};

TEST_F(ReduceArrayLiteralsTest, empty_array) {
  // our optimization doesn't bother with empty arrays
  auto code_str = R"(
    (
      (const v0 0)
      (new-array v0 "[I")
      (move-result-pseudo-object v1)
      (return-object v1)
    )
  )";
  const auto& expected_str = code_str;
  test(code_str, expected_str, 0, 0);
}

TEST_F(ReduceArrayLiteralsTest, illegal_aput_on_empty_array) {
  // this could would crash at runtime, but shouldn't crash at compile time
  auto code_str = R"(
    (
      (const v0 0)
      (new-array v0 "[Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (aput v2 v1 v0)
      (return-object v1)
    )
  )";
  const auto& expected_str = code_str;
  test(code_str, expected_str, 0, 0);
}

TEST_F(ReduceArrayLiteralsTest, array_one_element) {
  auto code_str = R"(
    (
      (const v0 1)
      (new-array v0 "[Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (const v0 0)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (aput-object v2 v1 v0)
      (return-object v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 1)
      (const v0 0)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (check-cast v2 "Ljava/lang/String;")
      (move-result-pseudo-object v3)
      (filled-new-array (v3) "[Ljava/lang/String;")
      (move-result-object v1)
      (return-object v1)
    )
  )";
  test(code_str, expected_str, 1, 1);
}

TEST_F(ReduceArrayLiteralsTest, jelly_bean_x86) {
  // non-primitive elements before KitKat on x86 were buggy, and we bail
  auto code_str = R"(
    (
      (const v0 1)
      (new-array v0 "[Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (const v0 0)
      (const-string "hello jelly bean")
      (move-result-pseudo-object v2)
      (aput v2 v1 v0)
      (return-object v1)
    )
  )";
  const auto expected_str = code_str;
  test(code_str, expected_str, 0, 0, 222, 18, Architecture::X86);
}

/* TODO: Re-enable after cleaning up exclusions in ReduceArrayLiteral
TEST_F(ReduceArrayLiteralsTest, jelly_bean_armv7) {
  // non-primitive elements on non-x86 architectures always worked
  auto code_str = R"(
    (
      (const v0 1)
      (new-array v0 "[Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (const v0 0)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (aput v2 v1 v0)
      (return-object v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 1)
      (const v0 0)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (move-object v3 v2)
      (filled-new-array (v3) "[Ljava/lang/String;")
      (move-result-object v1)
      (return-object v1)
    )
  )";
  test(code_str, expected_str, 1, 1, 222, 18, Architecture::ARMV7);
}
*/

TEST_F(ReduceArrayLiteralsTest, array_one_wide_element) {
  // wide arrays are not supported according to spec
  auto code_str = R"(
    (
      (const v0 1)
      (new-array v0 "[J")
      (move-result-pseudo-object v1)
      (const v0 0)
      (const-wide v2 0)
      (aput-wide v2 v1 v0)
      (return-object v1)
    )
  )";
  const auto& expected_str = code_str;
  test(code_str, expected_str, 0, 0);
}

TEST_F(ReduceArrayLiteralsTest, array_one_boolean_element) {
  // non-int primitive arrays happen to be not implemented
  auto code_str = R"(
    (
      (const v0 1)
      (new-array v0 "[B")
      (move-result-pseudo-object v1)
      (const v0 0)
      (const v2 0)
      (aput-boolean v2 v1 v0)
      (return-object v1)
    )
  )";
  const auto& expected_str = code_str;
  test(code_str, expected_str, 0, 0);
}

TEST_F(ReduceArrayLiteralsTest, array_one_cyclic_element) {
  // storing the array itself in it amounts to escaping
  auto code_str = R"(
    (
      (const v0 1)
      (new-array v0 "[Ljava.lang.Object;")
      (move-result-pseudo-object v1)
      (const v0 0)
      (aput-wide v1 v1 v0)
      (return-object v1)
    )
  )";
  const auto& expected_str = code_str;
  test(code_str, expected_str, 0, 0);
}

TEST_F(ReduceArrayLiteralsTest, array_more_than_max_elements) {
  DexMethod::make_method(
      "Ljava/lang/System;.arraycopy:"
      "(Ljava/lang/Object;ILjava/lang/Object;II)V");

  auto code_str = R"(
    (
      (const v0 2)
      (new-array v0 "[Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (const v0 0)
      (aput-object v2 v1 v0)
      (const-string "hello2")
      (move-result-pseudo-object v2)
      (const v0 1)
      (aput-object v2 v1 v0)
      (return-object v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 2)
      (new-array v0 "[Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (const v0 0)
      (check-cast v2 "Ljava/lang/String;")
      (move-result-pseudo-object v7)
      (filled-new-array (v7) "[Ljava/lang/String;")
      (move-result-object v3)
      (const v4 0)
      (const v5 0)
      (const v6 1)
      (invoke-static (v3 v4 v1 v5 v6) "Ljava/lang/System;.arraycopy:(Ljava/lang/Object;ILjava/lang/Object;II)V")
      (const-string "hello2")
      (move-result-pseudo-object v2)
      (const v0 1)
      (check-cast v2 "Ljava/lang/String;")
      (move-result-pseudo-object v7)
      (filled-new-array (v7) "[Ljava/lang/String;")
      (move-result-object v3)
      (const v4 0)
      (const v5 1)
      (const v6 1)
      (invoke-static (v3 v4 v1 v5 v6) "Ljava/lang/System;.arraycopy:(Ljava/lang/Object;ILjava/lang/Object;II)V")
      (return-object v1)
    )
  )";
  test(code_str, expected_str, 1, 2, 1);
}

TEST_F(ReduceArrayLiteralsTest, array_two_same_elements) {
  auto code_str = R"(
    (
      (const v0 2)
      (new-array v0 "[Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (const v0 0)
      (aput-object v2 v1 v0)
      (const v0 1)
      (aput-object v2 v1 v0)
      (return-object v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 2)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (const v0 0)
      (check-cast v2 "Ljava/lang/String;")
      (move-result-pseudo-object v3)
      (const v0 1)
      (check-cast v2 "Ljava/lang/String;")
      (move-result-pseudo-object v4)
      (filled-new-array (v3 v4) "[Ljava/lang/String;")
      (move-result-object v1)
      (return-object v1)
    )
  )";
  test(code_str, expected_str, 1, 2);
}

TEST_F(ReduceArrayLiteralsTest, array_two_different_elements) {
  auto code_str = R"(
    (
      (const v0 2)
      (new-array v0 "[Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (const v0 0)
      (aput-object v2 v1 v0)
      (const-string "hello2")
      (move-result-pseudo-object v2)
      (const v0 1)
      (aput-object v2 v1 v0)
      (return-object v1)
    )
  )";
  const auto& expected_str = R"(
    (
      (const v0 2)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (const v0 0)
      (check-cast v2 "Ljava/lang/String;")
      (move-result-pseudo-object v3)
      (const-string "hello2")
      (move-result-pseudo-object v2)
      (const v0 1)
      (check-cast v2 "Ljava/lang/String;")
      (move-result-pseudo-object v4)
      (filled-new-array (v3 v4) "[Ljava/lang/String;")
      (move-result-object v1)
      (return-object v1)
    )
  )";
  test(code_str, expected_str, 1, 2);
}

TEST_F(ReduceArrayLiteralsTest, conditional_escape) {
  auto code_str = R"(
    (
      (load-param v3)
      (const v0 1)
      (new-array v0 "[Ljava/lang/String;")
      (move-result-pseudo-object v1)
      (if-eqz v3 :skip_aput)
      (const v0 0)
      (const-string "hello")
      (move-result-pseudo-object v2)
      (aput v2 v1 v0)
      (:skip_aput)
      (return-object v1)
    )
  )";
  const auto& expected_str = code_str;
  test(code_str, expected_str, 0, 0);
}
