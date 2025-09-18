/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexUtil.h"
#include "PrintKotlinStats.h"
#include "RedexTest.h"

class KotlinStatsTest : public RedexIntegrationTest {};

namespace {
TEST_F(KotlinStatsTest, MethodHasNoEqDefined) {
  auto* klr = new PrintKotlinStats();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  PrintKotlinStats::Stats stats = klr->get_stats();

  EXPECT_EQ(stats.kotlin_null_check_insns, 9);
  EXPECT_EQ(stats.kotlin_public_param_objects, 21);

  // LExample;.$$delegatedProperties:[Lkotlin/reflect/KProperty;
  // LFooDelagates;.lazyValue$delegate:Lkotlin/Lazy;
  // Lfoo;.unsafeLazy:Lkotlin/Lazy;
  EXPECT_EQ(stats.kotlin_delegates, 1);
  EXPECT_EQ(stats.kotlin_lazy_delegates, 2);

  // LKotlinLambdaInline$foo$1;
  // LFooDelagates$lazyValue$2;
  EXPECT_EQ(stats.kotlin_lambdas, 2);

  // LKotlinLambdaInline$foo$1;
  EXPECT_EQ(stats.kotlin_class_with_instance, 1);

  // LKotlinLambdaInline$foo$1;
  EXPECT_EQ(stats.kotlin_non_capturing_lambda, 1);

  // LDelegate1;
  // LKotlinLambdaInline$foo$1;
  // LKotlinLayzyKt;
  // LCompanionClass$Companion;
  // LKotlinLambdaInline;
  // LKotlinDefaultArgs;
  // LCompanionClass;
  // LDelegateTest;
  // LExample;
  // LAnotherCompanionClass$Test;
  // LFooDelagates$lazyValue$2;
  // LFooDelagates;
  // LKotlinCompanionObjKt;
  // Lfoo;
  // LAnotherCompanionClass;
  EXPECT_EQ(stats.kotlin_class, 15);

  // Named companion object is not counted yet
  // LCompanionClass$Companion;
  EXPECT_EQ(stats.kotlin_companion_class, 1);

  // LKotlinLambdaInline$foo$1;
  // LFooDelagates$lazyValue$2;
  EXPECT_EQ(stats.kotlin_anonymous_class, 2);

  // LKotlinDefaultArgs.greet$default, with 2 default args
  EXPECT_EQ(stats.kotlin_default_arg_method, 1);
  EXPECT_EQ(stats.kotlin_default_arg_check_insns, 2);
  EXPECT_EQ(stats.kotlin_and_lit_insns, 2);
}
} // namespace
