/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexAsm.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "RedexTest.h"
#include "RemoveApiLevelChecks.h"

using namespace cfg;
using namespace dex_asm;

class RemoveApiLevelChecksTest : public RedexTest {
 public:
  size_t run(IRCode* code, int32_t min_sdk) {
    const auto* sdk_int_field = RemoveApiLevelChecksPass::get_sdk_int_field();
    code->build_cfg();
    auto res = RemoveApiLevelChecksPass::run(code, min_sdk, sdk_int_field);
    code->clear_cfg();
    return res.num_removed;
  }

  ::testing::AssertionResult run_fail(const std::string& code_str,
                                      int32_t min_sdk) {
    auto code_s_expr = get_s_expr(code_str);
    auto code = assembler::ircode_from_string(code_str);

    size_t removed = run(code.get(), min_sdk);
    if (removed != 0u) {
      return ::testing::AssertionFailure() << "Removed " << removed << "blocks";
    }
    auto res_expr = assembler::to_s_expr(code.get());
    if (res_expr.str() != code_s_expr.str()) {
      return ::testing::AssertionFailure()
             << "Code was changed: " << code_s_expr.str() << " became "
             << res_expr.str();
    }
    return ::testing::AssertionSuccess();
  }

  ::testing::AssertionResult run_success(const std::string& code_str,
                                         int32_t min_sdk,
                                         const std::string& expected_str,
                                         size_t expected_removed = 1u) {
    auto code = assembler::ircode_from_string(code_str);

    size_t removed = run(code.get(), min_sdk);
    if (removed != expected_removed) {
      return ::testing::AssertionFailure() << "Removed " << removed << "blocks";
    }
    auto res_expr = assembler::to_s_expr(code.get());

    auto exp_s_expr = get_s_expr(expected_str);

    if (res_expr.str() != exp_s_expr.str()) {
      return ::testing::AssertionFailure()
             << "Code not as expected: " << res_expr.str() << " vs "
             << exp_s_expr.str();
    }
    return ::testing::AssertionSuccess();
  }

  ::sparta::s_expr get_s_expr(const std::string& code) {
    return assembler::to_s_expr(assembler::ircode_from_string(code).get());
  }

  static std::string get_conditional_code(const std::string& if_code) {
    return R"(
     (
      (load-param v0)

      (sget "Landroid/os/Build$VERSION;.SDK_INT:I")
      (move-result-pseudo v0)

      (const v1 25)

      ()" + if_code +
           R"( :if-true-label)
      (const v1 0)
      (return-void)

      (:if-true-label)
      (const v1 1)
      (return-void)
     )
    )";
  }

  static std::string get_true_result() {
    return R"(
     (
      (load-param v0)

      (sget "Landroid/os/Build$VERSION;.SDK_INT:I")
      (move-result-pseudo v0)

      (const v1 25)

      (const v1 1)
      (return-void)
     )
    )";
  }

  static std::string get_false_result() {
    return R"(
     (
      (load-param v0)

      (sget "Landroid/os/Build$VERSION;.SDK_INT:I")
      (move-result-pseudo v0)

      (const v1 25)

      (const v1 0)
      (return-void)
     )
    )";
  }
};

TEST_F(RemoveApiLevelChecksTest, LtLHS) {
  // "min_sdk < 25 ?"
  auto code = get_conditional_code("if-lt v0 v1");

  EXPECT_TRUE(run_fail(code, 23));
  EXPECT_TRUE(run_fail(code, 24));
  EXPECT_TRUE(run_success(code, 25, get_false_result()));
  EXPECT_TRUE(run_success(code, 26, get_false_result()));
}

TEST_F(RemoveApiLevelChecksTest, LeLHS) {
  // "min_sdk <= 25 ?"
  auto code = get_conditional_code("if-le v0 v1");

  EXPECT_TRUE(run_fail(code, 24));
  EXPECT_TRUE(run_fail(code, 25));
  EXPECT_TRUE(run_success(code, 26, get_false_result()));
  EXPECT_TRUE(run_success(code, 27, get_false_result()));
}

TEST_F(RemoveApiLevelChecksTest, GtLHS) {
  // "min_sdk > 25 ?"
  auto code = get_conditional_code("if-gt v0 v1");

  EXPECT_TRUE(run_fail(code, 24));
  EXPECT_TRUE(run_fail(code, 25));
  EXPECT_TRUE(run_success(code, 26, get_true_result()));
  EXPECT_TRUE(run_success(code, 27, get_true_result()));
}

TEST_F(RemoveApiLevelChecksTest, GeLHS) {
  // "min_sdk >= 25 ?"
  auto code = get_conditional_code("if-ge v0 v1");

  EXPECT_TRUE(run_fail(code, 23));
  EXPECT_TRUE(run_fail(code, 24));
  EXPECT_TRUE(run_success(code, 25, get_true_result()));
  EXPECT_TRUE(run_success(code, 26, get_true_result()));
}

TEST_F(RemoveApiLevelChecksTest, LtRHS) {
  // "25 < min_sdk ?"
  auto code = get_conditional_code("if-lt v1 v0");

  EXPECT_TRUE(run_fail(code, 24));
  EXPECT_TRUE(run_fail(code, 25));
  EXPECT_TRUE(run_success(code, 26, get_true_result()));
  EXPECT_TRUE(run_success(code, 27, get_true_result()));
}

TEST_F(RemoveApiLevelChecksTest, LeRHS) {
  // "25 <= min_sdk ?"
  auto code = get_conditional_code("if-le v1 v0");

  EXPECT_TRUE(run_fail(code, 23));
  EXPECT_TRUE(run_fail(code, 24));
  EXPECT_TRUE(run_success(code, 25, get_true_result()));
  EXPECT_TRUE(run_success(code, 26, get_true_result()));
}

TEST_F(RemoveApiLevelChecksTest, GtRHS) {
  // "25 > min_sdk ?"
  auto code = get_conditional_code("if-gt v1 v0");

  EXPECT_TRUE(run_fail(code, 23));
  EXPECT_TRUE(run_fail(code, 24));
  EXPECT_TRUE(run_success(code, 25, get_false_result()));
  EXPECT_TRUE(run_success(code, 26, get_false_result()));
}

TEST_F(RemoveApiLevelChecksTest, GeRHS) {
  // "25 >= min_sdk ?"
  auto code = get_conditional_code("if-ge v1 v0");

  EXPECT_TRUE(run_fail(code, 24));
  EXPECT_TRUE(run_fail(code, 25));
  EXPECT_TRUE(run_success(code, 26, get_false_result()));
  EXPECT_TRUE(run_success(code, 27, get_false_result()));
}

TEST_F(RemoveApiLevelChecksTest, Unary) {
  auto code = get_conditional_code("if-eqz v0");
  EXPECT_TRUE(run_fail(code, 0));
  EXPECT_TRUE(run_success(code, 1, get_false_result()));

  code = get_conditional_code("if-nez v0");
  EXPECT_TRUE(run_fail(code, 0));
  EXPECT_TRUE(run_success(code, 1, get_true_result()));

  code = get_conditional_code("if-ltz v0");
  EXPECT_TRUE(run_fail(code, -1));
  EXPECT_TRUE(run_success(code, 0, get_false_result()));

  code = get_conditional_code("if-lez v0");
  EXPECT_TRUE(run_fail(code, 0));
  EXPECT_TRUE(run_success(code, 1, get_false_result()));

  code = get_conditional_code("if-gtz v0");
  EXPECT_TRUE(run_fail(code, 0));
  EXPECT_TRUE(run_success(code, 1, get_true_result()));

  code = get_conditional_code("if-gez v0");
  EXPECT_TRUE(run_fail(code, -1));
  EXPECT_TRUE(run_success(code, 0, get_true_result()));
}
