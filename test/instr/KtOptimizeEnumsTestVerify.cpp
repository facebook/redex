/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <unordered_set>

#include "Debug.h"
#include "DexInstruction.h"
#include "SwitchMap.h"
#include "verify/VerifyUtil.h"

namespace {

constexpr const char* name_WhenMappings =
    "Lcom/facebook/redextest/kt/OptimizeEnumsTestKt$WhenMappings;";
constexpr const char* name_A = "Lcom/facebook/redextest/kt/A;";
constexpr const char* name_B = "Lcom/facebook/redextest/kt/B;";
constexpr const char* name_Big = "Lcom/facebook/redextest/kt/Big;";

constexpr const char* name_useA =
    "Lcom/facebook/redextest/kt/OptimizeEnumsTestKt;.useA:("
    "Lcom/facebook/redextest/kt/A;"
    ")I";

constexpr const char* name_useB =
    "Lcom/facebook/redextest/kt/OptimizeEnumsTestKt;.useB:("
    "Lcom/facebook/redextest/kt/B;"
    ")I";

constexpr const char* name_useAAgain =
    "Lcom/facebook/redextest/kt/OptimizeEnumsTestKt;.useAAgain:("
    "Lcom/facebook/redextest/kt/A;"
    ")I";

std::unordered_set<size_t> collect_switch_cases(DexMethodRef* method_ref) {
  auto method = static_cast<DexMethod*>(method_ref);
  method->balloon();

  auto code = method->get_code();
  std::unordered_set<size_t> switch_cases;

  auto smp =
      SwitchMethodPartitioning::create(code, /* verify_default_case */ false);
  redex_assert(smp);
  for (const auto& entry : smp->get_key_to_block()) {
    switch_cases.insert(entry.first);
  }
  return switch_cases;
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

  auto switch_cases_A = collect_switch_cases(meth_useA);
  auto switch_cases_B = collect_switch_cases(meth_useB);
  auto switch_cases_A_again = collect_switch_cases(meth_useAAgain);

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

  auto switch_cases_A = collect_switch_cases(meth_useA);
  auto switch_cases_B = collect_switch_cases(meth_useB);
  auto switch_cases_A_again = collect_switch_cases(meth_useAAgain);

  // OptimizeEnumsPass replaces the old keys with ordinals. Here we check if the
  // keys are expected.
  std::unordered_set<size_t> expected_switch_cases_A{0, 2};
  std::unordered_set<size_t> expected_switch_cases_B{0, 2};
  std::unordered_set<size_t> expected_switch_cases_A_again{0, 1};

  EXPECT_EQ(expected_switch_cases_A, switch_cases_A);
  EXPECT_EQ(expected_switch_cases_B, switch_cases_B);
  EXPECT_EQ(expected_switch_cases_A_again, switch_cases_A_again);
}
