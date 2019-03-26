/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <unordered_set>

#include "DexInstruction.h"
#include "SwitchMap.h"
#include "verify/VerifyUtil.h"

namespace {

constexpr const char* FOO = "Lcom/facebook/redextest/Foo;";
constexpr const char* FOO_ANONYMOUS = "Lcom/facebook/redextest/Foo$1;";
constexpr const char* ENUM_A = "Lcom/facebook/redextest/EnumA;";
constexpr const char* ENUM_B = "Lcom/facebook/redextest/EnumB;";
constexpr const char* BIG_ENUM = "Lcom/facebook/redextest/BigEnum;";

std::unordered_set<size_t> collect_switch_cases(DexMethodRef* method_ref) {
  auto method = static_cast<DexMethod*>(method_ref);
  method->balloon();

  auto code = method->get_code();
  std::unordered_set<size_t> switch_cases;

  SwitchMethodPartitioning smp(code, /* verify_default_case */ false);
  for (const auto& entry : smp.get_key_to_block()) {
    switch_cases.insert(entry.first);
  }
  return switch_cases;
}

} // namespace

TEST_F(PreVerify, GeneratedClass) {
  auto enumA = find_class_named(classes, ENUM_A);
  EXPECT_NE(nullptr, enumA);

  auto enumB = find_class_named(classes, ENUM_B);
  EXPECT_NE(nullptr, enumB);

  auto bigEnum = find_class_named(classes, BIG_ENUM);
  EXPECT_NE(nullptr, bigEnum);

  auto foo = find_class_named(classes, FOO);
  EXPECT_NE(nullptr, foo);

  auto foo_anonymous = find_class_named(classes, FOO_ANONYMOUS);
  EXPECT_NE(nullptr, foo_anonymous);

  auto method_use_enumA = DexMethod::get_method(
      "Lcom/facebook/redextest/Foo;.useEnumA:(Lcom/facebook/redextest/"
      "EnumA;)I");
  auto switch_cases_A = collect_switch_cases(method_use_enumA);
  std::unordered_set<size_t> expected_switch_cases_A = {1, 2};
  EXPECT_EQ(expected_switch_cases_A, switch_cases_A);

  auto method_use_enumB = DexMethod::get_method(
      "Lcom/facebook/redextest/Foo;.useEnumB:(Lcom/facebook/redextest/"
      "EnumB;)I");
  auto switch_cases_B = collect_switch_cases(method_use_enumB);
  std::unordered_set<size_t> expected_switch_cases_B = {1, 2};
  EXPECT_EQ(expected_switch_cases_B, switch_cases_B);

  auto method_use_enumA_again = DexMethod::get_method(
      "Lcom/facebook/redextest/Foo;.useEnumA_again:(Lcom/facebook/redextest/"
      "EnumA;)I");
  auto switch_cases_A_again = collect_switch_cases(method_use_enumA_again);
  std::unordered_set<size_t> expected_switch_cases_A_again = {1, 3};
  auto code = static_cast<DexMethod*>(method_use_enumA_again)->get_code();
  code->build_cfg();
  EXPECT_EQ(expected_switch_cases_A_again, switch_cases_A_again)
      << show(code->cfg());
}

TEST_F(PostVerify, GeneratedClass) {
  auto enumA = find_class_named(classes, ENUM_A);
  EXPECT_NE(nullptr, enumA);

  auto enumB = find_class_named(classes, ENUM_B);
  EXPECT_NE(nullptr, enumB);

  auto bigEnum = find_class_named(classes, BIG_ENUM);
  EXPECT_NE(nullptr, bigEnum);

  auto foo = find_class_named(classes, FOO);
  EXPECT_NE(nullptr, foo);

  auto foo_anonymous = find_class_named(classes, FOO_ANONYMOUS);
  EXPECT_EQ(nullptr, foo_anonymous);

  auto method_use_enumA = DexMethod::get_method(
      "Lcom/facebook/redextest/Foo;.useEnumA:(Lcom/facebook/redextest/"
      "EnumA;)I");
  auto switch_cases_A = collect_switch_cases(method_use_enumA);
  std::unordered_set<size_t> expected_switch_cases_A = {0, 2};
  EXPECT_EQ(expected_switch_cases_A, switch_cases_A);

  auto method_use_enumB = DexMethod::get_method(
      "Lcom/facebook/redextest/Foo;.useEnumB:(Lcom/facebook/redextest/"
      "EnumB;)I");
  auto switch_cases_B = collect_switch_cases(method_use_enumB);
  std::unordered_set<size_t> expected_switch_cases_B = {0, 2};
  EXPECT_EQ(expected_switch_cases_B, switch_cases_B);

  auto method_use_enumA_again = DexMethod::get_method(
      "Lcom/facebook/redextest/Foo;.useEnumA_again:(Lcom/facebook/redextest/"
      "EnumA;)I");
  auto switch_cases_A_again = collect_switch_cases(method_use_enumA_again);
  std::unordered_set<size_t> expected_switch_cases_A_again = {0, 1};
  EXPECT_EQ(expected_switch_cases_A_again, switch_cases_A_again);
}
