/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "GlobalTypeAnalysisPass.h"
#include "TypeAnalysisTestBase.h"

using namespace type_analyzer;
using namespace type_analyzer::global;

using TypeSet = sparta::PatriciaTreeSet<const DexType*>;

class TypeAnalysisTransformTest : public TypeAnalysisTestBase {};

TEST_F(TypeAnalysisTransformTest, RemoveRedundantNullCheckTest) {
  auto scope = build_class_scope(stores);
  set_root_method(
      "Lcom/facebook/redextest/TestRemoveRedundantNullChecks;.main:()V");

  auto gta = new GlobalTypeAnalysisPass();
  gta->get_config().transform.remove_redundant_null_checks = true;
  std::vector<Pass*> passes{gta};
  run_passes(passes);

  {
    auto meth_check_null_arg =
        get_method("TestRemoveRedundantNullChecks;.checkEQZNullArg",
                   "Lcom/facebook/redextest/Base;", "I");
    auto code = meth_check_null_arg->get_code();
    const auto& insns = InstructionIterable(code);
    for (auto& mie : insns) {
      EXPECT_NE(mie.insn->opcode(), OPCODE_IF_EQZ);
    }
  }
  {
    auto meth_check_not_null_arg =
        get_method("TestRemoveRedundantNullChecks;.checkEQZNotNullArg",
                   "Lcom/facebook/redextest/Base;", "I");
    auto code = meth_check_not_null_arg->get_code();
    const auto& insns = InstructionIterable(code);
    for (auto& mie : insns) {
      EXPECT_NE(mie.insn->opcode(), OPCODE_IF_EQZ);
    }
  }
  {
    auto meth_check_null_arg =
        get_method("TestRemoveRedundantNullChecks;.checkNEZNullArg",
                   "Lcom/facebook/redextest/Base;", "I");
    auto code = meth_check_null_arg->get_code();
    const auto& insns = InstructionIterable(code);
    for (auto& mie : insns) {
      EXPECT_NE(mie.insn->opcode(), OPCODE_IF_NEZ);
    }
  }
  {
    auto meth_check_null_arg =
        get_method("TestRemoveRedundantNullChecks;.checkNEZNotNullArg",
                   "Lcom/facebook/redextest/Base;", "I");
    auto code = meth_check_null_arg->get_code();
    const auto& insns = InstructionIterable(code);
    for (auto& mie : insns) {
      EXPECT_NE(mie.insn->opcode(), OPCODE_IF_NEZ);
    }
  }
  {
    auto meth_check_null_arg =
        get_method("TestRemoveRedundantNullChecks;.checkEQZInitReachable",
                   "Lcom/facebook/redextest/Base;", "I");
    auto code = meth_check_null_arg->get_code();
    const auto& insns = InstructionIterable(code);
    bool found_if_eqz = false;
    for (auto& mie : insns) {
      if (mie.insn->opcode() == OPCODE_IF_EQZ) {
        found_if_eqz = true;
      }
    }
    EXPECT_TRUE(found_if_eqz);
  }
  {
    auto meth_check_null_arg = get_method(
        "TestRemoveRedundantNullChecks;.checkEQZInitReachableGetField", "",
        "I");
    auto code = meth_check_null_arg->get_code();
    const auto& insns = InstructionIterable(code);
    bool found_if_eqz = false;
    for (auto& mie : insns) {
      if (mie.insn->opcode() == OPCODE_IF_EQZ) {
        found_if_eqz = true;
      }
    }
    EXPECT_TRUE(found_if_eqz);
  }
  {
    auto meth_check_base_field =
        get_method("RenderView;.checkBaseField", "", "I");
    auto code = meth_check_base_field->get_code();
    const auto& insns = InstructionIterable(code);
    bool found_if_eqz = false;
    for (auto& mie : insns) {
      if (mie.insn->opcode() == OPCODE_IF_EQZ) {
        found_if_eqz = true;
      }
    }
    EXPECT_TRUE(found_if_eqz);
  }
  {
    auto meth_check_primitive_array_field =
        get_method("ReactNode;.getCool", "I", "Ljava/lang/String;");
    auto code = meth_check_primitive_array_field->get_code();
    const auto& insns = InstructionIterable(code);
    bool found_if_eqz = false;
    for (auto& mie : insns) {
      if (mie.insn->opcode() == OPCODE_IF_EQZ) {
        found_if_eqz = true;
      }
    }
    EXPECT_TRUE(found_if_eqz);
  }
}

TEST_F(TypeAnalysisTransformTest, TestRemoveRedundantTypeChecks) {
  auto scope = build_class_scope(stores);
  set_root_method(
      "Lcom/facebook/redextest/TestRemoveRedundantTypeChecks;.main:()V");

  auto gta = new GlobalTypeAnalysisPass();
  gta->get_config().transform.remove_redundant_type_checks = true;
  std::vector<Pass*> passes{gta};
  run_passes(passes);

  {
    auto meth_check_null_arg =
        get_method("TestRemoveRedundantTypeChecks;.checkInstanceOfBaseNullArg",
                   "Lcom/facebook/redextest/Base;", "I");
    auto code = meth_check_null_arg->get_code();
    const auto& insns = InstructionIterable(code);
    bool found_const_0 = false;
    for (auto& mie : insns) {
      auto insn = mie.insn;
      EXPECT_NE(insn->opcode(), OPCODE_INSTANCE_OF);
      if (insn->opcode() == OPCODE_CONST && insn->get_literal() == 0) {
        found_const_0 = true;
      }
    }
    EXPECT_TRUE(found_const_0);
  }
  {
    auto meth_check_not_null_arg = get_method(
        "TestRemoveRedundantTypeChecks;.checkInstanceOfBaseNotNullArg",
        "Lcom/facebook/redextest/Base;", "I");
    auto code = meth_check_not_null_arg->get_code();
    const auto& insns = InstructionIterable(code);
    bool found_const_1 = false;
    for (auto& mie : insns) {
      auto insn = mie.insn;
      EXPECT_NE(insn->opcode(), OPCODE_INSTANCE_OF);
      if (insn->opcode() == OPCODE_CONST && insn->get_literal() == 1) {
        found_const_1 = true;
      }
    }
    EXPECT_TRUE(found_const_1);
  }
  {
    auto meth_check_sub_one =
        get_method("TestRemoveRedundantTypeChecks;.checkInstanceOfSubOneArg",
                   "Lcom/facebook/redextest/Base;", "I");
    auto code = meth_check_sub_one->get_code();
    const auto& insns = InstructionIterable(code);
    bool found_const_1 = false;
    for (auto& mie : insns) {
      auto insn = mie.insn;
      EXPECT_NE(insn->opcode(), OPCODE_INSTANCE_OF);
      if (insn->opcode() == OPCODE_CONST && insn->get_literal() == 1) {
        found_const_1 = true;
      }
    }
    EXPECT_TRUE(found_const_1);
  }
  {
    auto meth_check_sub_two =
        get_method("TestRemoveRedundantTypeChecks;.checkInstanceOfSubTwoArg",
                   "Lcom/facebook/redextest/Base;", "I");
    auto code = meth_check_sub_two->get_code();
    const auto& insns = InstructionIterable(code);
    bool found_const_0 = false;
    for (auto& mie : insns) {
      auto insn = mie.insn;
      EXPECT_NE(insn->opcode(), OPCODE_INSTANCE_OF);
      if (insn->opcode() == OPCODE_CONST && insn->get_literal() == 0) {
        found_const_0 = true;
      }
    }
    EXPECT_TRUE(found_const_0);
  }
  {
    auto meth_check_nullable_field = get_method(
        "TestRemoveRedundantTypeChecks;.checkInstanceOfNullableField", "", "I");
    auto code = meth_check_nullable_field->get_code();
    const auto& insns = InstructionIterable(code);
    bool found_instance_of = false;
    for (auto& mie : insns) {
      auto insn = mie.insn;
      if (insn->opcode() == OPCODE_INSTANCE_OF) {
        found_instance_of = true;
      }
    }
    EXPECT_TRUE(found_instance_of);
  }
}
