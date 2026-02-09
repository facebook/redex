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

using ::testing::IsNull;
using ::testing::NotNull;

namespace {
constexpr std::string_view test_class = "LKotlinLambdaDedupNonSingletonTest;";

// Lambda classes for the first group of lambdas (3 - meets threshold).
constexpr std::string_view lambda1 =
    "LKotlinLambdaDedupNonSingletonTest$useLambda1$1;";
constexpr std::string_view lambda2 =
    "LKotlinLambdaDedupNonSingletonTest$useLambda2$1;";
constexpr std::string_view lambda3 =
    "LKotlinLambdaDedupNonSingletonTest$useLambda3$1;";

// Lambda classes for the second group (4 - above threshold).
constexpr std::string_view second_group_lambda1 =
    "LKotlinLambdaDedupNonSingletonTest$useSecondGroupLambda1$1;";
constexpr std::string_view second_group_lambda2 =
    "LKotlinLambdaDedupNonSingletonTest$useSecondGroupLambda2$1;";
constexpr std::string_view second_group_lambda3 =
    "LKotlinLambdaDedupNonSingletonTest$useSecondGroupLambda3$1;";
constexpr std::string_view second_group_lambda4 =
    "LKotlinLambdaDedupNonSingletonTest$useSecondGroupLambda4$1;";

// Lambda class for the unique lambda (only 1 instance).
constexpr std::string_view unique_lambda =
    "LKotlinLambdaDedupNonSingletonTest$useUniqueLambda$1;";

// Lambda classes for the below-threshold group (2 - below threshold).
constexpr std::string_view below_threshold_lambda1 =
    "LKotlinLambdaDedupNonSingletonTest$useBelowThresholdLambda1$1;";
constexpr std::string_view below_threshold_lambda2 =
    "LKotlinLambdaDedupNonSingletonTest$useBelowThresholdLambda2$1;";

// Extract the new-instance type referenced by a method.
// Returns nullptr if no new-instance instruction is found.
const DexType* get_new_instance_type(const DexMethod* m) {
  const auto* code = m->get_dex_code();
  if (code == nullptr) {
    return nullptr;
  }
  const auto& insns = code->get_instructions();
  auto it = std::ranges::find_if(insns, [](const auto& insn) {
    return insn->opcode() == DOPCODE_NEW_INSTANCE;
  });
  if (it == insns.end()) {
    return nullptr;
  }
  return dynamic_cast<const DexOpcodeType*>(*it)->get_type();
}

// Extract the invoke-direct <init> method reference.
// Returns nullptr if no invoke-direct to <init> is found.
const DexMethodRef* get_invoke_direct_init(const DexMethod* m) {
  const auto* code = m->get_dex_code();
  if (code == nullptr) {
    return nullptr;
  }
  const auto& insns = code->get_instructions();
  auto it = std::ranges::find_if(insns, [](const auto& insn) {
    if (insn->opcode() != DOPCODE_INVOKE_DIRECT) {
      return false;
    }
    const auto* meth = dynamic_cast<const DexOpcodeMethod*>(insn)->get_method();
    return meth->get_name()->str() == "<init>";
  });
  if (it == insns.end()) {
    return nullptr;
  }
  return dynamic_cast<const DexOpcodeMethod*>(*it)->get_method();
}

// Extract the sget-object field referenced by a method.
// Returns nullptr if no sget-object instruction is found.
DexFieldRef* get_sget_field(const DexMethod* m) {
  const auto* code = m->get_dex_code();
  if (code == nullptr) {
    return nullptr;
  }
  const auto& insns = code->get_instructions();
  auto it = std::ranges::find_if(insns, [](const auto& insn) {
    return insn->opcode() == DOPCODE_SGET_OBJECT;
  });
  if (it == insns.end()) {
    return nullptr;
  }
  return dynamic_cast<const DexOpcodeField*>(*it)->get_field();
}

// Check if a type belongs to a duplicate group.
bool is_group_member_type(
    const DexType* type,
    std::initializer_list<std::string_view> group_lambdas) {
  return std::ranges::any_of(group_lambdas, [type](std::string_view lambda) {
    return type == DexType::get_type(lambda);
  });
}

// Get the invoke method's code for a lambda class.
// Note: We use find_vmethod_named instead of KotlinLambdaAnalyzer because
// KotlinLambdaAnalyzer::get_invoke_method() checks get_code() != nullptr, but
// in PreVerify tests get_code() returns nullptr.
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

// Compare two method codes for structural equality.
bool codes_equal(const DexCode* a, const DexCode* b) {
  always_assert(a != nullptr);
  always_assert(b != nullptr);
  return std::ranges::equal(
      a->get_instructions(), b->get_instructions(),
      [](const auto& x, const auto& y) { return *x == *y; });
}
} // namespace

// Parameterized test fixture for sanity checking singleton removal.
class SanityCheckSingletonRemoved
    : public PostVerify,
      public ::testing::WithParamInterface<std::string_view> {};

TEST_P(SanityCheckSingletonRemoved, UsesNewInstance) {
  // After KotlinStatelessLambdaSingletonRemovalPass, lambdas should use
  // new-instance instead of sget-object.
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  std::string_view method_name = GetParam();
  const auto* method = find_vmethod_named(*cls, method_name);
  ASSERT_THAT(method, NotNull()) << "Method not found: " << method_name;

  // Should NOT use sget-object anymore
  const auto* field = get_sget_field(method);
  EXPECT_THAT(field, IsNull())
      << "Method " << method_name << " still uses sget-object";

  // Should use new-instance instead
  const auto* type = get_new_instance_type(method);
  EXPECT_THAT(type, NotNull())
      << "Method " << method_name << " does not use new-instance";
}

INSTANTIATE_TEST_SUITE_P(PostVerify,
                         SanityCheckSingletonRemoved,
                         ::testing::Values("useLambda1",
                                           "useLambda2",
                                           "useLambda3",
                                           "useSecondGroupLambda1",
                                           "useSecondGroupLambda2",
                                           "useSecondGroupLambda3",
                                           "useSecondGroupLambda4",
                                           "useUniqueLambda",
                                           "useBelowThresholdLambda1",
                                           "useBelowThresholdLambda2"));

TEST_F(PreVerify, LambdaClassesExist) {
  auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  EXPECT_THAT(find_class_named(classes, lambda1), NotNull());
  EXPECT_THAT(find_class_named(classes, lambda2), NotNull());
  EXPECT_THAT(find_class_named(classes, lambda3), NotNull());
  EXPECT_THAT(find_class_named(classes, second_group_lambda1), NotNull());
  EXPECT_THAT(find_class_named(classes, second_group_lambda2), NotNull());
  EXPECT_THAT(find_class_named(classes, second_group_lambda3), NotNull());
  EXPECT_THAT(find_class_named(classes, second_group_lambda4), NotNull());
  EXPECT_THAT(find_class_named(classes, unique_lambda), NotNull());
  EXPECT_THAT(find_class_named(classes, below_threshold_lambda1), NotNull());
  EXPECT_THAT(find_class_named(classes, below_threshold_lambda2), NotNull());
}

TEST_F(PreVerify, SanityCheckFirstGroupLambdasIdentical) {
  const auto* code1 = get_invoke_code(classes, lambda1);
  const auto* code2 = get_invoke_code(classes, lambda2);
  const auto* code3 = get_invoke_code(classes, lambda3);
  ASSERT_THAT(code1, NotNull());
  EXPECT_TRUE(codes_equal(code1, code2));
  EXPECT_TRUE(codes_equal(code1, code3));
}

TEST_F(PreVerify, SanityCheckSecondGroupLambdasIdentical) {
  const auto* code1 = get_invoke_code(classes, second_group_lambda1);
  const auto* code2 = get_invoke_code(classes, second_group_lambda2);
  const auto* code3 = get_invoke_code(classes, second_group_lambda3);
  const auto* code4 = get_invoke_code(classes, second_group_lambda4);
  ASSERT_THAT(code1, NotNull());
  EXPECT_TRUE(codes_equal(code1, code2));
  EXPECT_TRUE(codes_equal(code1, code3));
  EXPECT_TRUE(codes_equal(code1, code4));
}

TEST_F(PostVerify, NonSingletonLambdasDeduplicated) {
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  const auto* use1 = find_vmethod_named(*cls, "useLambda1");
  const auto* use2 = find_vmethod_named(*cls, "useLambda2");
  const auto* use3 = find_vmethod_named(*cls, "useLambda3");
  ASSERT_THAT(use1, NotNull());
  ASSERT_THAT(use2, NotNull());
  ASSERT_THAT(use3, NotNull());

  const auto* type1 = get_new_instance_type(use1);
  const auto* type2 = get_new_instance_type(use2);
  const auto* type3 = get_new_instance_type(use3);
  ASSERT_THAT(type1, NotNull());
  ASSERT_THAT(type2, NotNull());
  ASSERT_THAT(type3, NotNull());

  {
    SCOPED_TRACE("All should reference the same canonical type");
    EXPECT_EQ(type1, type2);
    EXPECT_EQ(type1, type3);
  }
  EXPECT_TRUE(is_group_member_type(type1, {lambda1, lambda2, lambda3}))
      << "The type should be one of the group's lambda types";

  const auto* ctor1 = get_invoke_direct_init(use1);
  const auto* ctor2 = get_invoke_direct_init(use2);
  const auto* ctor3 = get_invoke_direct_init(use3);
  ASSERT_THAT(ctor1, NotNull());
  ASSERT_THAT(ctor2, NotNull());
  ASSERT_THAT(ctor3, NotNull());
  {
    SCOPED_TRACE("Constructor should reference the same canonical class");
    EXPECT_EQ(ctor1->get_class(), type1);
    EXPECT_EQ(ctor2->get_class(), type1);
    EXPECT_EQ(ctor3->get_class(), type1);
  }
}

TEST_F(PostVerify, SecondGroupDeduplicated) {
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  const auto* sg1 = find_vmethod_named(*cls, "useSecondGroupLambda1");
  const auto* sg2 = find_vmethod_named(*cls, "useSecondGroupLambda2");
  const auto* sg3 = find_vmethod_named(*cls, "useSecondGroupLambda3");
  const auto* sg4 = find_vmethod_named(*cls, "useSecondGroupLambda4");
  ASSERT_THAT(sg1, NotNull());
  ASSERT_THAT(sg2, NotNull());
  ASSERT_THAT(sg3, NotNull());
  ASSERT_THAT(sg4, NotNull());

  const auto* type1 = get_new_instance_type(sg1);
  const auto* type2 = get_new_instance_type(sg2);
  const auto* type3 = get_new_instance_type(sg3);
  const auto* type4 = get_new_instance_type(sg4);
  ASSERT_THAT(type1, NotNull());
  ASSERT_THAT(type2, NotNull());
  ASSERT_THAT(type3, NotNull());
  ASSERT_THAT(type4, NotNull());

  {
    SCOPED_TRACE("All should reference the same canonical type");
    EXPECT_EQ(type1, type2);
    EXPECT_EQ(type1, type3);
    EXPECT_EQ(type1, type4);
  }
  EXPECT_TRUE(
      is_group_member_type(type1, {second_group_lambda1, second_group_lambda2,
                                   second_group_lambda3, second_group_lambda4}))
      << "The type should be one of the group's lambda types";

  const auto* ctor1 = get_invoke_direct_init(sg1);
  const auto* ctor2 = get_invoke_direct_init(sg2);
  const auto* ctor3 = get_invoke_direct_init(sg3);
  const auto* ctor4 = get_invoke_direct_init(sg4);
  ASSERT_THAT(ctor1, NotNull());
  ASSERT_THAT(ctor2, NotNull());
  ASSERT_THAT(ctor3, NotNull());
  ASSERT_THAT(ctor4, NotNull());
  {
    SCOPED_TRACE("Constructor should reference the same canonical class");
    EXPECT_EQ(ctor1->get_class(), type1);
    EXPECT_EQ(ctor2->get_class(), type1);
    EXPECT_EQ(ctor3->get_class(), type1);
    EXPECT_EQ(ctor4->get_class(), type1);
  }
}

TEST_F(PostVerify, DifferentGroupsUseDifferentCanonicals) {
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  const auto* first = find_vmethod_named(*cls, "useLambda1");
  const auto* second = find_vmethod_named(*cls, "useSecondGroupLambda1");
  ASSERT_THAT(first, NotNull());
  ASSERT_THAT(second, NotNull());

  const auto* first_type = get_new_instance_type(first);
  const auto* second_type = get_new_instance_type(second);
  ASSERT_THAT(first_type, NotNull());
  ASSERT_THAT(second_type, NotNull());

  EXPECT_NE(first_type, second_type)
      << "Different groups should use different canonical lambdas";
}

TEST_F(PostVerify, BelowThresholdLambdasNotDeduplicated) {
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  const auto* bt1 = find_vmethod_named(*cls, "useBelowThresholdLambda1");
  const auto* bt2 = find_vmethod_named(*cls, "useBelowThresholdLambda2");
  ASSERT_THAT(bt1, NotNull());
  ASSERT_THAT(bt2, NotNull());

  const auto* type1 = get_new_instance_type(bt1);
  const auto* type2 = get_new_instance_type(bt2);
  ASSERT_THAT(type1, NotNull());
  ASSERT_THAT(type2, NotNull());

  {
    SCOPED_TRACE("Should not be deduplicated - should reference its own class");
    EXPECT_EQ(type1, DexType::get_type(below_threshold_lambda1));
    EXPECT_EQ(type2, DexType::get_type(below_threshold_lambda2));
  }

  const auto* ctor1 = get_invoke_direct_init(bt1);
  const auto* ctor2 = get_invoke_direct_init(bt2);
  ASSERT_THAT(ctor1, NotNull());
  ASSERT_THAT(ctor2, NotNull());
  {
    SCOPED_TRACE("Constructor should reference its own class");
    EXPECT_EQ(ctor1->get_class(), type1);
    EXPECT_EQ(ctor2->get_class(), type2);
  }
}

TEST_F(PostVerify, UniqueLambdaNotDeduplicated) {
  const auto* cls = find_class_named(classes, test_class);
  ASSERT_THAT(cls, NotNull());

  const auto* unique = find_vmethod_named(*cls, "useUniqueLambda");
  ASSERT_THAT(unique, NotNull());

  const auto* type = get_new_instance_type(unique);
  ASSERT_THAT(type, NotNull());
  EXPECT_EQ(type, DexType::get_type(unique_lambda))
      << "Should not be deduplicated - should reference its own class";

  const auto* ctor = get_invoke_direct_init(unique);
  ASSERT_THAT(ctor, NotNull());
  EXPECT_EQ(ctor->get_class(), type)
      << "Constructor should reference its own class";
}
