/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Armv7JitLoopChecker.h"
#include "ControlFlow.h"
#include "verify/VerifyUtil.h"

// armv7_jit_loop_checker_fail runs the same input with armv7_jit_loop_max_score
// set to exactly this score, and expects Redex to fail.
TEST_F(PostVerify, ParserLoopScore) {
  auto* cls = find_class_named(
      classes, "Lcom/facebook/redextest/Armv7JitLoopCheckerTest;");
  ASSERT_NE(cls, nullptr);
  auto* method = find_dmethod_named(*cls, "parse");
  ASSERT_NE(method, nullptr);
  method->balloon();
  auto* code = method->get_code();
  code->build_cfg();

  auto score = Armv7JitLoopChecker::score_method(code->cfg(), {});
  ASSERT_TRUE(score);
  // f0..f54 and the loop index, across the 55 cases and the default.
  EXPECT_EQ(score->carried_values, 56u);
  EXPECT_EQ(score->merge_in_degree, 56u);
  EXPECT_EQ(score->score, 3136u);
}
