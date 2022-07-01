/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexUtil.h"
#include "PrintKotlinStats.h"
#include "RedexTest.h"
#include "Resolver.h"
#include "RewriteKotlinSingletonInstance.h"
#include "Show.h"

class KotlinStatsTest : public RedexIntegrationTest {};

namespace {
TEST_F(KotlinStatsTest, MethodHasNoEqDefined) {
  auto klr = new PrintKotlinStats();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  PrintKotlinStats::Stats stats = klr->get_stats();

  ASSERT_EQ(stats.kotlin_null_check_insns, 9);
  ASSERT_EQ(stats.kotlin_public_param_objects, 21);

  // LExample;.$$delegatedProperties:[Lkotlin/reflect/KProperty;
  // LFooDelagates;.lazyValue$delegate:Lkotlin/Lazy;
  // Lfoo;.unsafeLazy:Lkotlin/Lazy;
  ASSERT_EQ(stats.kotlin_delegates, 1);
  ASSERT_EQ(stats.kotlin_lazy_delegates, 2);

  // LKotlinLambdaInline$foo$1;
  // LFooDelagates$lazyValue$2;
  ASSERT_EQ(stats.kotlin_lambdas, 2);

  // LKotlinLambdaInline$foo$1;
  ASSERT_EQ(stats.kotlin_class_with_instance, 1);

  // LKotlinLambdaInline$foo$1;
  ASSERT_EQ(stats.kotlin_non_capturing_lambda, 1);

  // LDelegate1;
  // LKotlinLambdaInline$foo$1;
  // LKotlinLayzyKt;
  // LCompanionClass$Companion;
  // LKotlinLambdaInline;
  // LCompanionClass;
  // LDelegateTest;
  // LExample;
  // LAnotherCompanionClass$Test;
  // LFooDelagates$lazyValue$2;
  // LFooDelagates;
  // LKotlinCompanionObjKt;
  // Lfoo;
  // LAnotherCompanionClass;
  ASSERT_EQ(stats.kotlin_class, 14);

  // Named companion object is not counted yet
  // LCompanionClass$Companion;
  ASSERT_EQ(stats.kotlin_companion_class, 1);

  // LKotlinLambdaInline$foo$1;
  // LFooDelagates$lazyValue$2;
  ASSERT_EQ(stats.kotlin_anonymous_class, 2);
}
} // namespace
