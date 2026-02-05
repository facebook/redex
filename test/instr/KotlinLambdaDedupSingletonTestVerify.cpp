/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "verify/VerifyUtil.h"

#include <algorithm>
#include <initializer_list>
#include <ranges>
#include <string_view>

#include <gmock/gmock.h>

#include "KotlinLambdaDeduplicationPass.h"

using ::testing::IsNull;
using ::testing::NotNull;

namespace {
constexpr std::string_view test_class = "LKotlinLambdaDedupSingletonTest;";

// Lambda classes for the first group of trivial lambdas (3 - meets threshold).
// These should be deduplicated.
constexpr std::string_view trivial_lambda1 =
    "LKotlinLambdaDedupSingletonTest$useTrivialLambda1$1;";
constexpr std::string_view trivial_lambda2 =
    "LKotlinLambdaDedupSingletonTest$useTrivialLambda2$1;";
constexpr std::string_view trivial_lambda3 =
    "LKotlinLambdaDedupSingletonTest$useTrivialLambda3$1;";

// Lambda classes for the second group of trivial lambdas (4 - above threshold).
// These should be deduplicated to a DIFFERENT canonical lambda.
constexpr std::string_view second_group_lambda1 =
    "LKotlinLambdaDedupSingletonTest$useSecondGroupLambda1$1;";
constexpr std::string_view second_group_lambda2 =
    "LKotlinLambdaDedupSingletonTest$useSecondGroupLambda2$1;";
constexpr std::string_view second_group_lambda3 =
    "LKotlinLambdaDedupSingletonTest$useSecondGroupLambda3$1;";
constexpr std::string_view second_group_lambda4 =
    "LKotlinLambdaDedupSingletonTest$useSecondGroupLambda4$1;";

// Lambda class for the unique lambda (only 1 instance).
// Should NOT be deduplicated.
constexpr std::string_view unique_lambda =
    "LKotlinLambdaDedupSingletonTest$useUniqueLambda$1;";

// Lambda classes for the below-threshold group (2 - below
// min_duplicate_group_size). Should NOT be deduplicated.
constexpr std::string_view below_threshold_lambda1 =
    "LKotlinLambdaDedupSingletonTest$useBelowThresholdLambda1$1;";
constexpr std::string_view below_threshold_lambda2 =
    "LKotlinLambdaDedupSingletonTest$useBelowThresholdLambda2$1;";

// Lambda classes for the third group of lambdas (3 - meets threshold).
// These should be deduplicated.
constexpr std::string_view third_group_lambda1 =
    "LKotlinLambdaDedupSingletonTest$useNonTrivialLambda1$1;";
constexpr std::string_view third_group_lambda2 =
    "LKotlinLambdaDedupSingletonTest$useNonTrivialLambda2$1;";
constexpr std::string_view third_group_lambda3 =
    "LKotlinLambdaDedupSingletonTest$useNonTrivialLambda3$1;";

// Extract the sget-object field referenced by a method.
// Returns nullptr if no sget-object instruction is found.
DexFieldRef* get_sget_field(const DexMethod* m) {
  const auto* code = m->get_dex_code();
  if (code == nullptr) {
    return nullptr;
  }
  for (const auto& insn : code->get_instructions()) {
    if (insn->opcode() == DOPCODE_SGET_OBJECT) {
      return dynamic_cast<const DexOpcodeField*>(insn)->get_field();
    }
  }
  return nullptr;
}

// Get the invoke method's code for a lambda class.
//
// We don't use type::get_kotlin_lambda_invoke_method here because it requires
// deobfuscated names, which are not available in PreVerify tests.
const DexCode* get_invoke_code(const Scope& classes,
                               std::string_view lambda_class) {
  const auto* cls = find_class_named(classes, lambda_class);
  if (cls == nullptr) {
    return nullptr;
  }
  const auto* invoke = find_vmethod_named(*cls, "invoke");
  if (invoke == nullptr) {
    return nullptr;
  }
  return invoke->get_dex_code();
}

// Compare two method codes for structural equality using
// DexInstruction::operator==. This mirrors how
// UniqueMethodTracker::cfg_code_equals compares IRInstructions.
bool codes_equal(const DexCode* a, const DexCode* b) {
  always_assert(a != nullptr);
  always_assert(b != nullptr);
  return std::ranges::equal(
      a->get_instructions(), b->get_instructions(),
      [](const auto& x, const auto& y) { return *x == *y; });
}

// Check if a field is a deduplicated INSTANCE field.
bool is_deduped_instance_field(const DexFieldRef* field) {
  return field != nullptr &&
         field->str() == KotlinLambdaDeduplicationPass::kDedupedInstanceName;
}

// Check if a field's class belongs to a duplicate group and is deduplicated.
bool is_deduped_group_member_instance(
    const DexFieldRef* field,
    std::initializer_list<std::string_view> group_lambdas) {
  if (!is_deduped_instance_field(field)) {
    return false;
  }
  return std::ranges::any_of(group_lambdas, [field](std::string_view lambda) {
    return field->get_class() == DexType::get_type(lambda);
  });
}
} // namespace

TEST_F(PreVerify, LambdaClassesExist) {
  auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  EXPECT_THAT(find_class_named(classes, trivial_lambda1), NotNull());
  EXPECT_THAT(find_class_named(classes, trivial_lambda2), NotNull());
  EXPECT_THAT(find_class_named(classes, trivial_lambda3), NotNull());
  EXPECT_THAT(find_class_named(classes, second_group_lambda1), NotNull());
  EXPECT_THAT(find_class_named(classes, second_group_lambda2), NotNull());
  EXPECT_THAT(find_class_named(classes, second_group_lambda3), NotNull());
  EXPECT_THAT(find_class_named(classes, second_group_lambda4), NotNull());
  EXPECT_THAT(find_class_named(classes, unique_lambda), NotNull());
  EXPECT_THAT(find_class_named(classes, below_threshold_lambda1), NotNull());
  EXPECT_THAT(find_class_named(classes, below_threshold_lambda2), NotNull());
  EXPECT_THAT(find_class_named(classes, third_group_lambda1), NotNull());
  EXPECT_THAT(find_class_named(classes, third_group_lambda2), NotNull());
  EXPECT_THAT(find_class_named(classes, third_group_lambda3), NotNull());
}

TEST_F(PreVerify, SanityCheckTrivialLambdasIdentical) {
  // Sanity check: trivial lambdas in the first group should be identical
  const auto* code1 = get_invoke_code(classes, trivial_lambda1);
  const auto* code2 = get_invoke_code(classes, trivial_lambda2);
  const auto* code3 = get_invoke_code(classes, trivial_lambda3);
  ASSERT_THAT(code1, NotNull());
  EXPECT_TRUE(codes_equal(code1, code2));
  EXPECT_TRUE(codes_equal(code1, code3));
}

TEST_F(PreVerify, SanityCheckSecondGroupLambdasIdentical) {
  // Sanity check: second group lambdas should be identical
  const auto* code1 = get_invoke_code(classes, second_group_lambda1);
  const auto* code2 = get_invoke_code(classes, second_group_lambda2);
  const auto* code3 = get_invoke_code(classes, second_group_lambda3);
  const auto* code4 = get_invoke_code(classes, second_group_lambda4);
  ASSERT_THAT(code1, NotNull());
  EXPECT_TRUE(codes_equal(code1, code2));
  EXPECT_TRUE(codes_equal(code1, code3));
  EXPECT_TRUE(codes_equal(code1, code4));
}

TEST_F(PreVerify, SanityCheckThirdGroupLambdasIdentical) {
  // Sanity check: Third group lambdas should be identical to each other
  const auto* code1 = get_invoke_code(classes, third_group_lambda1);
  const auto* code2 = get_invoke_code(classes, third_group_lambda2);
  const auto* code3 = get_invoke_code(classes, third_group_lambda3);
  ASSERT_THAT(code1, NotNull());
  EXPECT_TRUE(codes_equal(code1, code2));
  EXPECT_TRUE(codes_equal(code1, code3));
}

TEST_F(PreVerify, SanityCheckDifferentGroupsAreDifferent) {
  // Sanity check: Different lambda groups should have different code
  const auto* trivial = get_invoke_code(classes, trivial_lambda1);
  const auto* second = get_invoke_code(classes, second_group_lambda1);
  ASSERT_THAT(trivial, NotNull());
  ASSERT_THAT(second, NotNull());
  EXPECT_FALSE(codes_equal(trivial, second));
}

TEST_F(PostVerify, LambdasDeduplicated) {
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  // First group (3 lambdas) - all should reference the same canonical
  // lambda's INSTANCE$redex$dedup field
  const auto* use1 = find_vmethod_named(*cls, "useTrivialLambda1");
  const auto* use2 = find_vmethod_named(*cls, "useTrivialLambda2");
  const auto* use3 = find_vmethod_named(*cls, "useTrivialLambda3");
  ASSERT_THAT(use1, NotNull());
  ASSERT_THAT(use2, NotNull());
  ASSERT_THAT(use3, NotNull());

  auto* field1 = get_sget_field(use1);
  auto* field2 = get_sget_field(use2);
  auto* field3 = get_sget_field(use3);
  ASSERT_THAT(field1, NotNull());
  ASSERT_THAT(field2, NotNull());
  ASSERT_THAT(field3, NotNull());

  // All should reference the same deduplicated field
  EXPECT_EQ(field1, field2);
  EXPECT_EQ(field1, field3);

  // The field should be one of the group's lambdas' INSTANCE$redex$dedup
  EXPECT_TRUE(is_deduped_group_member_instance(
      field1, {trivial_lambda1, trivial_lambda2, trivial_lambda3}));

  // Second group (4 lambdas) - all should reference the same canonical field
  const auto* sg1 = find_vmethod_named(*cls, "useSecondGroupLambda1");
  const auto* sg2 = find_vmethod_named(*cls, "useSecondGroupLambda2");
  const auto* sg3 = find_vmethod_named(*cls, "useSecondGroupLambda3");
  const auto* sg4 = find_vmethod_named(*cls, "useSecondGroupLambda4");
  ASSERT_THAT(sg1, NotNull());
  ASSERT_THAT(sg2, NotNull());
  ASSERT_THAT(sg3, NotNull());
  ASSERT_THAT(sg4, NotNull());

  auto* sg_field1 = get_sget_field(sg1);
  auto* sg_field2 = get_sget_field(sg2);
  auto* sg_field3 = get_sget_field(sg3);
  auto* sg_field4 = get_sget_field(sg4);
  ASSERT_THAT(sg_field1, NotNull());
  ASSERT_THAT(sg_field2, NotNull());
  ASSERT_THAT(sg_field3, NotNull());
  ASSERT_THAT(sg_field4, NotNull());

  EXPECT_EQ(sg_field1, sg_field2);
  EXPECT_EQ(sg_field1, sg_field3);
  EXPECT_EQ(sg_field1, sg_field4);

  EXPECT_TRUE(is_deduped_group_member_instance(
      sg_field1, {second_group_lambda1, second_group_lambda2,
                  second_group_lambda3, second_group_lambda4}));

  // Third group (3 lambdas) - all should reference the same canonical field
  const auto* tg1 = find_vmethod_named(*cls, "useNonTrivialLambda1");
  const auto* tg2 = find_vmethod_named(*cls, "useNonTrivialLambda2");
  const auto* tg3 = find_vmethod_named(*cls, "useNonTrivialLambda3");
  ASSERT_THAT(tg1, NotNull());
  ASSERT_THAT(tg2, NotNull());
  ASSERT_THAT(tg3, NotNull());

  auto* tg_field1 = get_sget_field(tg1);
  auto* tg_field2 = get_sget_field(tg2);
  auto* tg_field3 = get_sget_field(tg3);
  ASSERT_THAT(tg_field1, NotNull());
  ASSERT_THAT(tg_field2, NotNull());
  ASSERT_THAT(tg_field3, NotNull());

  EXPECT_EQ(tg_field1, tg_field2);
  EXPECT_EQ(tg_field1, tg_field3);

  EXPECT_TRUE(is_deduped_group_member_instance(
      tg_field1,
      {third_group_lambda1, third_group_lambda2, third_group_lambda3}));
}

TEST_F(PostVerify, DifferentGroupsUseDifferentCanonicals) {
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  const auto* trivial = find_vmethod_named(*cls, "useTrivialLambda1");
  const auto* second = find_vmethod_named(*cls, "useSecondGroupLambda1");
  ASSERT_THAT(trivial, NotNull());
  ASSERT_THAT(second, NotNull());

  auto* trivial_field = get_sget_field(trivial);
  auto* second_field = get_sget_field(second);
  ASSERT_THAT(trivial_field, NotNull());
  ASSERT_THAT(second_field, NotNull());

  // Different groups should use different canonical lambdas
  EXPECT_NE(trivial_field, second_field);
  EXPECT_NE(trivial_field->get_class(), second_field->get_class());
}

TEST_F(PostVerify, BelowThresholdLambdasNotDeduplicated) {
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  // Below-threshold lambdas are identical but only 2 instances (below
  // min_duplicate_group_size=3). They should still reference their original
  // lambda INSTANCE fields.
  const auto* bt1 = find_vmethod_named(*cls, "useBelowThresholdLambda1");
  const auto* bt2 = find_vmethod_named(*cls, "useBelowThresholdLambda2");
  ASSERT_THAT(bt1, NotNull());
  ASSERT_THAT(bt2, NotNull());

  auto* field1 = get_sget_field(bt1);
  auto* field2 = get_sget_field(bt2);
  ASSERT_THAT(field1, NotNull());
  ASSERT_THAT(field2, NotNull());

  // Should NOT be deduplicated - each should reference its own class
  EXPECT_EQ(field1->get_class(), DexType::get_type(below_threshold_lambda1));
  EXPECT_EQ(field2->get_class(), DexType::get_type(below_threshold_lambda2));

  // Fields should NOT be renamed (still named "INSTANCE")
  EXPECT_FALSE(is_deduped_instance_field(field1));
  EXPECT_FALSE(is_deduped_instance_field(field2));
}

TEST_F(PostVerify, UniqueLambdaNotDeduplicated) {
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  // Unique lambda (only 1 instance) should still reference its original
  // lambda INSTANCE field.
  const auto* unique = find_vmethod_named(*cls, "useUniqueLambda");
  ASSERT_THAT(unique, NotNull());

  auto* field = get_sget_field(unique);
  ASSERT_THAT(field, NotNull());

  // Should NOT be deduplicated - should reference its own class
  EXPECT_EQ(field->get_class(), DexType::get_type(unique_lambda));

  // Field should NOT be renamed (still named "INSTANCE")
  EXPECT_FALSE(is_deduped_instance_field(field));
}
