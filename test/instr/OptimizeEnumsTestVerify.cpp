/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <unordered_set>

#include "DexInstruction.h"
#include "verify/VerifyUtil.h"

namespace {

constexpr const char* FOO = "Lcom/facebook/redextest/Foo;";
constexpr const char* FOO_ANONYMOUS = "Lcom/facebook/redextest/Foo$1;";
constexpr const char* ENUM_A = "Lcom/facebook/redextest/EnumA;";
constexpr const char* ENUM_B = "Lcom/facebook/redextest/EnumB;";

std::unordered_set<size_t> collect_switch_cases(DexMethodRef* method_ref) {
  auto method = static_cast<DexMethod*>(method_ref);
  method->balloon();

  auto code = method->get_code();
  std::unordered_set<size_t> switch_cases;
  IRInstruction* packed_switch_insn = nullptr;

  for (auto it = code->begin(); it != code->end(); ++it) {
    if (it->type == MFLOW_OPCODE) {
      auto insn = it->insn;
      if (insn->opcode() == OPCODE_PACKED_SWITCH ||
          insn->opcode() == OPCODE_SPARSE_SWITCH) {
        // We assume there is only 1 switch statement.
        EXPECT_EQ(nullptr, packed_switch_insn);
        packed_switch_insn = it->insn;
      }
    }

    if (it->type == MFLOW_TARGET) {
      auto branch_target = static_cast<BranchTarget*>(it->target);

      if (branch_target->type == BRANCH_MULTI &&
          branch_target->src != nullptr &&
          branch_target->src->type == MFLOW_OPCODE &&
          branch_target->src->insn == packed_switch_insn) {
        switch_cases.emplace(branch_target->case_key);
      }
    }
  }

  return switch_cases;
}

} // namespace

TEST_F(PreVerify, GeneratedClass) {
  auto enumA = find_class_named(classes, ENUM_A);
  EXPECT_NE(nullptr, enumA);

  auto foo = find_class_named(classes, FOO);
  EXPECT_NE(nullptr, foo);
  auto foo_anonymous = find_class_named(classes, FOO_ANONYMOUS);
  EXPECT_NE(nullptr, foo_anonymous);

  auto enumB = find_class_named(classes, ENUM_B);
  EXPECT_NE(nullptr, enumB);

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
  std::unordered_set<size_t> expected_switch_cases_A_again = {2, 1, 3};
  EXPECT_EQ(expected_switch_cases_A_again, switch_cases_A_again);
}

TEST_F(PostVerify, GeneratedClass) {
  auto enumA = find_class_named(classes, ENUM_A);
  EXPECT_NE(nullptr, enumA);

  auto enumB = find_class_named(classes, ENUM_B);
  EXPECT_NE(nullptr, enumB);

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
  std::unordered_set<size_t> expected_switch_cases_A_again = {2, 0, 1};
  EXPECT_EQ(expected_switch_cases_A_again, switch_cases_A_again);
}
