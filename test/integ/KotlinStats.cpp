/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexUtil.h"
#include "PrintKotlinStats.h"
#include "RedexTest.h"

using ::testing::NotNull;

class KotlinStatsTest : public RedexIntegrationTest {};

namespace {
TEST_F(KotlinStatsTest, MethodHasNoEqDefined) {
  // Set deobfuscated names for lambda classes so is_kotlin_lambda works.
  // Integration tests don't set deobfuscated names by default.
  const std::vector<const char*> lambda_class_names = {
      "LKotlinLambdaInline$foo$1;",
      "LKotlinLambdaInline$bar$1;",
      "LKotlinLambdaInline$baz$1;",
  };
  for (const auto* name : lambda_class_names) {
    auto* cls = type_class(DexType::get_type(name));
    ASSERT_THAT(cls, NotNull()) << "Lambda class not found: " << name;
    cls->set_deobfuscated_name(name);
  }

  auto* klr = new PrintKotlinStats();
  std::vector<Pass*> passes{klr};
  run_passes(passes);

  PrintKotlinStats::Stats stats = klr->get_stats();

  EXPECT_EQ(stats.kotlin_null_check_insns, 9);
  EXPECT_EQ(stats.kotlin_public_param_objects, 29);

  // LExample;.$$delegatedProperties:[Lkotlin/reflect/KProperty;
  // LFooDelagates;.lazyValue$delegate:Lkotlin/Lazy;
  // Lfoo;.unsafeLazy:Lkotlin/Lazy;
  EXPECT_EQ(stats.kotlin_delegates, 1);
  EXPECT_EQ(stats.kotlin_lazy_delegates, 2);

  // LKotlinLambdaInline$foo$1;
  // LKotlinLambdaInline$bar$1;
  // LKotlinLambdaInline$baz$1;
  // LFooDelagates$lazyValue$2;
  EXPECT_EQ(stats.kotlin_lambdas, 4);

  // LKotlinLambdaInline$foo$1;
  // LKotlinLambdaInline$bar$1;
  // LKotlinLambdaInline$baz$1;
  EXPECT_EQ(stats.kotlin_class_with_instance, 3);

  // LKotlinLambdaInline$foo$1;
  // LKotlinLambdaInline$bar$1;
  // LKotlinLambdaInline$baz$1;
  EXPECT_EQ(stats.kotlin_non_capturing_lambda, 3);

  // All three lambdas in LKotlinLambdaInline are trivial
  EXPECT_EQ(stats.kotlin_trivial_non_capturing_lambdas, 3u);
  // foo$1 and bar$1 have same body (a+b), baz$1 has different body (a-b)
  EXPECT_EQ(stats.kotlin_unique_trivial_non_capturing_lambdas, 2u);

  // LDelegate1;
  // LKotlinLambdaInline$foo$1;
  // LKotlinLambdaInline$bar$1;
  // LKotlinLambdaInline$baz$1;
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
  EXPECT_EQ(stats.kotlin_class, 17);

  // Named companion object is not counted yet
  // LCompanionClass$Companion;
  EXPECT_EQ(stats.kotlin_companion_class, 1);

  // LKotlinLambdaInline$foo$1;
  // LKotlinLambdaInline$bar$1;
  // LKotlinLambdaInline$baz$1;
  // LFooDelagates$lazyValue$2;
  EXPECT_EQ(stats.kotlin_anonymous_class, 4);

  // LKotlinDefaultArgs.greet$default, with 2 default args
  EXPECT_EQ(stats.kotlin_default_arg_method, 1);
  EXPECT_EQ(stats.kotlin_default_arg_check_insns, 2);
  EXPECT_EQ(stats.kotlin_and_lit_insns, 2);
}
} // namespace
