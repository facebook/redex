/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Debug.h"
#include "DexInstruction.h"
#include "SwitchMap.h"
#include "verify/OptimizeEnumCommon.h"
#include "verify/VerifyUtil.h"

namespace {

constexpr const char* name_WhenMappings =
    "Lcom/facebook/redextest/kt/OptimizeEnumSwitchMapTestKt$WhenMappings;";
constexpr const char* name_A = "Lcom/facebook/redextest/kt/A;";
constexpr const char* name_B = "Lcom/facebook/redextest/kt/B;";
constexpr const char* name_Big = "Lcom/facebook/redextest/kt/Big;";

constexpr const char* name_useA =
    "Lcom/facebook/redextest/kt/OptimizeEnumSwitchMapTestKt;.useA:("
    "Lcom/facebook/redextest/kt/A;"
    ")I";

constexpr const char* name_useB =
    "Lcom/facebook/redextest/kt/OptimizeEnumSwitchMapTestKt;.useB:("
    "Lcom/facebook/redextest/kt/B;"
    ")I";

constexpr const char* name_useAAgain =
    "Lcom/facebook/redextest/kt/OptimizeEnumSwitchMapTestKt;.useAAgain:("
    "Lcom/facebook/redextest/kt/A;"
    ")I";

constexpr const char* name_useBAgain =
    "Lcom/facebook/redextest/kt/OptimizeEnumSwitchMapTestKt;.useBAgain:("
    "Lcom/facebook/redextest/kt/B;"
    ")I";

constexpr const char* name_useBig =
    "Lcom/facebook/redextest/kt/OptimizeEnumSwitchMapTestKt;.useBig:("
    "Lcom/facebook/redextest/kt/Big;"
    ")I";

constexpr const char* name_useBigAgain =
    "Lcom/facebook/redextest/kt/OptimizeEnumSwitchMapTestKt;.useBigAgain:("
    "Lcom/facebook/redextest/kt/Big;"
    ")I";

// Note: Different versions of compilers (javac/kotlinc/d8) can generate
// different keys. So we don't check the specific values of the keys in
// PreVerify, but only that they are positive and unique.
void expect_kotlin_switchmapping_of_size(
    const std::set<BranchCase>& branch_cases,
    size_t expected_size,
    bool expect_null) {
  std::set<int64_t> seen;

  const auto expected_source = expect_null ? BranchSource::ArrayGetOrConstMinus1
                                           : BranchSource::ArrayGet;
  for (const auto& [source, value] : branch_cases) {
    EXPECT_EQ(source, expected_source);

    if (value == -1) {
      EXPECT_TRUE(expect_null);
    } else if (value == 0) {
      EXPECT_TRUE(expect_null);

      // Zero is optionally present, so we'll leave it out.
      continue;
    } else {
      EXPECT_GT(value, 0);
    }

    EXPECT_TRUE(seen.emplace(value).second);
  }

  EXPECT_EQ(seen.size(), expected_size);
}

} // namespace

TEST_F(PreVerify, KotlinGeneratedClass) {
  auto* cls_A = find_class_named(classes, name_A);
  EXPECT_NE(nullptr, cls_A);

  auto* cls_B = find_class_named(classes, name_B);
  EXPECT_NE(nullptr, cls_B);

  auto* cls_WhenMappings = find_class_named(classes, name_WhenMappings);
  EXPECT_NE(nullptr, cls_WhenMappings);

  auto* meth_useA = DexMethod::get_method(name_useA);
  auto* meth_useB = DexMethod::get_method(name_useB);
  auto* meth_useAAgain = DexMethod::get_method(name_useAAgain);
  auto* meth_useBAgain = DexMethod::get_method(name_useBAgain);
  auto* meth_useBig = DexMethod::get_method(name_useBig);
  auto* meth_useBigAgain = DexMethod::get_method(name_useBigAgain);

  auto switch_cases_A = collect_const_branch_cases(meth_useA);
  auto switch_cases_B = collect_const_branch_cases(meth_useB);
  auto switch_cases_A_again = collect_const_branch_cases(meth_useAAgain);
  auto switch_cases_B_again = collect_const_branch_cases(meth_useBAgain);
  auto switch_cases_Big = collect_const_branch_cases(meth_useBig);
  auto switch_cases_Big_again = collect_const_branch_cases(meth_useBigAgain);

  expect_kotlin_switchmapping_of_size(
      switch_cases_A, 2, /* expect_null */ false);
  expect_kotlin_switchmapping_of_size(
      switch_cases_B, 2, /* expect_null */ false);
  expect_kotlin_switchmapping_of_size(
      switch_cases_A_again, 2, /* expect_null */ false);
  expect_kotlin_switchmapping_of_size(
      switch_cases_B_again, 3, /* expect_null */ true);
  expect_kotlin_switchmapping_of_size(
      switch_cases_Big, 20, /* expect_null */ false);
  expect_kotlin_switchmapping_of_size(
      switch_cases_Big_again, 21, /* expect_null */ true);
}

TEST_F(PostVerify, KotlinGeneratedClass) {
  auto* cls_A = find_class_named(classes, name_A);
  EXPECT_NE(nullptr, cls_A);

  auto* cls_B = find_class_named(classes, name_B);
  EXPECT_NE(nullptr, cls_B);

  auto* cls_Big = find_class_named(classes, name_Big);
  EXPECT_NE(nullptr, cls_Big);

  auto* cls_WhenMappings = find_class_named(classes, name_WhenMappings);
  EXPECT_NE(nullptr, cls_WhenMappings);

  auto* meth_useA = DexMethod::get_method(name_useA);
  auto* meth_useB = DexMethod::get_method(name_useB);
  auto* meth_useAAgain = DexMethod::get_method(name_useAAgain);
  auto* meth_useBAgain = DexMethod::get_method(name_useBAgain);
  auto* meth_useBig = DexMethod::get_method(name_useBig);
  auto* meth_useBigAgain = DexMethod::get_method(name_useBigAgain);

  auto switch_cases_A = collect_const_branch_cases(meth_useA);
  auto switch_cases_B = collect_const_branch_cases(meth_useB);
  auto switch_cases_A_again = collect_const_branch_cases(meth_useAAgain);
  auto switch_cases_B_again = collect_const_branch_cases(meth_useBAgain);
  auto switch_cases_Big = collect_const_branch_cases(meth_useBig);
  auto switch_cases_Big_again = collect_const_branch_cases(meth_useBigAgain);

  // OptimizeEnumsPass replaces the old keys with ordinals. Here we check if the
  // keys are expected.
  std::set<BranchCase> expected_switch_cases_A{{BranchSource::VirtualCall, 0},
                                               {BranchSource::VirtualCall, 1},
                                               {BranchSource::VirtualCall, 2}};
  std::set<BranchCase> expected_switch_cases_B{{BranchSource::VirtualCall, 0},
                                               {BranchSource::VirtualCall, 1},
                                               {BranchSource::VirtualCall, 2}};
  std::set<BranchCase> expected_switch_cases_A_again{
      {BranchSource::VirtualCall, 0}, {BranchSource::VirtualCall, 1}};
  std::set<BranchCase> expected_switch_cases_B_again{
      {BranchSource::ArrayGetOrConstMinus1, -1},
      {BranchSource::ArrayGetOrConstMinus1, 1},
      {BranchSource::ArrayGetOrConstMinus1, 2}};
  std::set<BranchCase> expected_switch_cases_Big;
  for (int64_t i = 0; i != 20; ++i) {
    expected_switch_cases_Big.emplace(BranchSource::VirtualCall, i);
  }
  std::set<BranchCase> expected_switch_cases_Big_again = {
      {BranchSource::ArrayGetOrConstMinus1, -1}};
  for (int64_t i = 0; i != 21; ++i) {
    expected_switch_cases_Big_again.emplace(BranchSource::ArrayGetOrConstMinus1,
                                            i);
  }

  EXPECT_EQ(expected_switch_cases_A, switch_cases_A);
  EXPECT_EQ(expected_switch_cases_B, switch_cases_B);
  EXPECT_EQ(expected_switch_cases_A_again, switch_cases_A_again);
  EXPECT_EQ(expected_switch_cases_B_again, switch_cases_B_again);
  EXPECT_EQ(expected_switch_cases_Big, switch_cases_Big);
  EXPECT_EQ(expected_switch_cases_Big_again, switch_cases_Big_again);
}
