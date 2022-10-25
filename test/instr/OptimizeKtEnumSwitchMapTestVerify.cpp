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

  auto switch_cases_A = collect_const_branch_cases(meth_useA);
  auto switch_cases_B = collect_const_branch_cases(meth_useB);
  auto switch_cases_A_again = collect_const_branch_cases(meth_useAAgain);

  // Note: Different versions of compilers (javac/kotlinc/d8) can generate
  // different keys. So we don't check the values of the keys in PreVerify.
  EXPECT_EQ(switch_cases_A.size(), 2);
  EXPECT_EQ(switch_cases_B.size(), 2);
  EXPECT_EQ(switch_cases_A_again.size(), 2);
}

TEST_F(PostVerify, KotlinGeneratedClass) {
  auto* cls_A = find_class_named(classes, name_A);
  EXPECT_NE(nullptr, cls_A);

  auto* cls_B = find_class_named(classes, name_B);
  EXPECT_NE(nullptr, cls_B);

  auto* cls_Big = find_class_named(classes, name_Big);
  EXPECT_NE(nullptr, cls_Big);

  auto* cls_WhenMappings = find_class_named(classes, name_WhenMappings);
  EXPECT_EQ(nullptr, cls_WhenMappings);

  auto* meth_useA = DexMethod::get_method(name_useA);
  auto* meth_useB = DexMethod::get_method(name_useB);
  auto* meth_useAAgain = DexMethod::get_method(name_useAAgain);

  auto switch_cases_A = collect_const_branch_cases(meth_useA);
  auto switch_cases_B = collect_const_branch_cases(meth_useB);
  auto switch_cases_A_again = collect_const_branch_cases(meth_useAAgain);

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

  EXPECT_EQ(expected_switch_cases_A, switch_cases_A);
  EXPECT_EQ(expected_switch_cases_B, switch_cases_B);
  EXPECT_EQ(expected_switch_cases_A_again, switch_cases_A_again);
}
