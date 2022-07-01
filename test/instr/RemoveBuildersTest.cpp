/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdint>
#include <cstdlib>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>

#include "DexInstruction.h"
#include "Match.h"
#include "VerifyUtil.h"

namespace {

void check_no_builder(DexMethod* method, DexType* builder_type) {
  auto& insns = method->get_dex_code()->get_instructions();

  for (const auto& insn : insns) {
    auto opcode = insn->opcode();

    if (opcode == DOPCODE_NEW_INSTANCE ||
        dex_opcode::is_invoke(insn->opcode())) {
      DexType* cls_type = static_cast<DexOpcodeType*>(insn)->get_type();
      EXPECT_NE(builder_type, cls_type);
    } else if (dex_opcode::is_iget(opcode) || dex_opcode::is_iput(opcode)) {
      DexFieldRef* field =
          static_cast<const DexOpcodeField*>(insn)->get_field();
      EXPECT_NE(builder_type, field->get_class());
    }
  }
}

void check_has_builder(DexMethod* method, DexType* builder_type) {
  const auto& insns = method->get_dex_code()->get_instructions();

  bool has_builder = false;
  for (const auto& insn : insns) {
    auto opcode = insn->opcode();

    if (opcode == DOPCODE_NEW_INSTANCE ||
        dex_opcode::is_invoke(insn->opcode())) {
      DexType* cls_type = static_cast<DexOpcodeType*>(insn)->get_type();
      if (builder_type == cls_type) {
        has_builder = true;
        break;
      }
    } else if (dex_opcode::is_iget(opcode) || dex_opcode::is_iput(opcode)) {
      DexFieldRef* field =
          static_cast<const DexOpcodeField*>(insn)->get_field();
      if (builder_type == field->get_class()) {
        has_builder = true;
        break;
      }
    }
  }
  EXPECT_TRUE(has_builder);
}

} // namespace

/*
 * Check builder is actually defined.
 */
TEST_F(PreVerify, RemoveFooBuilder) {
  auto foo = find_class_named(classes, "Lcom/facebook/redex/test/instr/Foo;");
  EXPECT_NE(nullptr, foo);

  auto foo_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Foo$Builder;");
  EXPECT_NE(nullptr, foo_builder);
}

/*
 * Ensure the builder was removed and all calls were appropriately
 * replaced / removed.
 */
TEST_F(PostVerify, RemoveFooBuilder) {
  auto foo = find_class_named(classes, "Lcom/facebook/redex/test/instr/Foo;");
  EXPECT_NE(nullptr, foo);

  // Check builder class was removed.
  auto foo_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Foo$Builder;");
  EXPECT_EQ(nullptr, foo_builder);

  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");

  auto initialize_method =
      find_vmethod_named(*using_no_escape_builders, "initializeFoo");
  EXPECT_NE(nullptr, initialize_method);

  auto initialize_more_arguments = find_vmethod_named(
      *using_no_escape_builders, "initializeFooWithMoreArguments");
  EXPECT_NE(nullptr, initialize_more_arguments);

  DexType* builder_type =
      DexType::get_type("Lcom/facebook/redex/test/instr/Foo$Builder;");

  check_no_builder(initialize_method, builder_type);
  check_no_builder(initialize_more_arguments, builder_type);
}

/*
 * Check builder is actually defined.
 */
TEST_F(PreVerify, RemoveBarBuilder) {
  auto bar = find_class_named(classes, "Lcom/facebook/redex/test/instr/Bar;");
  EXPECT_NE(nullptr, bar);

  auto bar_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Bar$Builder;");
  EXPECT_NE(nullptr, bar_builder);
}

/*
 * Ensure the builder was removed.
 */
TEST_F(PostVerify, RemoveBarBuilder) {
  auto bar = find_class_named(classes, "Lcom/facebook/redex/test/instr/Bar;");
  EXPECT_NE(nullptr, bar);

  auto bar_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Bar$Builder;");
  EXPECT_EQ(nullptr, bar_builder);

  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_bar =
      find_vmethod_named(*using_no_escape_builders, "initializeBar");
  auto initialize_bar_different_regs = find_vmethod_named(
      *using_no_escape_builders, "initializeBarDifferentRegs");
  auto initialize_bar_same_value_different_branches = find_vmethod_named(
      *using_no_escape_builders, "initializeBarDifferentBranchesSameValues");

  EXPECT_NE(nullptr, initialize_bar_different_regs);
  EXPECT_NE(nullptr, initialize_bar);
  EXPECT_NE(nullptr, initialize_bar_same_value_different_branches);

  // No build call.
  EXPECT_EQ(nullptr,
            find_invoke(initialize_bar_different_regs,
                        DOPCODE_INVOKE_VIRTUAL,
                        "build"));
  EXPECT_EQ(nullptr,
            find_invoke(initialize_bar, DOPCODE_INVOKE_VIRTUAL, "build"));
  EXPECT_EQ(nullptr,
            find_invoke(initialize_bar, DOPCODE_INVOKE_VIRTUAL, "build"));
}

TEST_F(PostVerify, RemoveBarBuilder_simpleCase) {
  auto bar = find_class_named(classes, "Lcom/facebook/redex/test/instr/Bar;");
  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_bar =
      find_vmethod_named(*using_no_escape_builders, "initializeBar");
  DexType* builder_type =
      DexType::get_type("Lcom/facebook/redex/test/instr/Bar$Builder;");

  // Check builder was properly removed from the initialize_bar.
  check_no_builder(initialize_bar, builder_type);
}

TEST_F(PostVerify, RemoveBarBuilder_differentRegs) {
  auto bar = find_class_named(classes, "Lcom/facebook/redex/test/instr/Bar;");
  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_bar_different_regs = find_vmethod_named(
      *using_no_escape_builders, "initializeBarDifferentRegs");
  DexType* builder_type =
      DexType::get_type("Lcom/facebook/redex/test/instr/Bar$Builder;");

  // Check builder was properly removed from the initialize_bar.
  check_no_builder(initialize_bar_different_regs, builder_type);

  // Check that the register that holds field's value gets initialized
  // with both values (will get initialized depending on the branch)
  auto insns =
      initialize_bar_different_regs->get_dex_code()->get_instructions();
  std::vector<uint16_t> values;
  std::vector<uint16_t> expected_values = {6, 7};

  uint16_t constant_reg = std::numeric_limits<uint16_t>::max();
  for (const auto& insn : insns) {
    auto opcode = insn->opcode();

    if (opcode == DOPCODE_CONST_4) {
      constant_reg = insn->dest();
      values.push_back(insn->get_literal());
    } else if (dex_opcode::is_invoke(opcode)) {
      auto invoked = static_cast<DexOpcodeMethod*>(insn)->get_method();
      if (invoked->get_class() == bar->get_type()) {
        EXPECT_EQ(constant_reg, insn->src(1));
      }
    }
  }

  EXPECT_EQ(expected_values, values);
}

/*
 * Check builder is actually defined.
 */
TEST_F(PreVerify, RemoveCarBuilder) {
  auto car = find_class_named(classes, "Lcom/facebook/redex/test/instr/Car;");
  EXPECT_NE(nullptr, car);

  auto car_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Car$Builder;");
  EXPECT_NE(nullptr, car_builder);

  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_null_model =
      find_vmethod_named(*using_no_escape_builders, "initializeNullCarModel");
  auto initialize_model_different = find_vmethod_named(
      *using_no_escape_builders, "initializeNullOrDefinedCarModel");

  EXPECT_NE(nullptr, initialize_null_model);
  EXPECT_NE(nullptr, initialize_model_different);

  DexType* builder_type =
      DexType::get_type("Lcom/facebook/redex/test/instr/Car$Builder;");
  check_has_builder(initialize_null_model, builder_type);
  check_has_builder(initialize_model_different, builder_type);
}

TEST_F(PostVerify, RemoveCarBuilder) {
  auto car = find_class_named(classes, "Lcom/facebook/redex/test/instr/Car;");
  EXPECT_NE(nullptr, car);

  auto car_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Car$Builder;");
  EXPECT_EQ(nullptr, car_builder);

  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_null_model =
      find_vmethod_named(*using_no_escape_builders, "initializeNullCarModel");
  auto initialize_model_different = find_vmethod_named(
      *using_no_escape_builders, "initializeNullOrDefinedCarModel");

  EXPECT_NE(nullptr, initialize_null_model);
  EXPECT_NE(nullptr, initialize_model_different);

  // Check builder was properly removed from the methods.
  DexType* builder_type =
      DexType::get_type("Lcom/facebook/redex/test/instr/Car$Builder;");
  check_no_builder(initialize_null_model, builder_type);
  check_no_builder(initialize_model_different, builder_type);
}

TEST_F(PostVerify, RemoveCarBuilder_uninitializedModel) {
  auto car = find_class_named(classes, "Lcom/facebook/redex/test/instr/Car;");
  auto car_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Car$Builder;");
  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_null_model =
      find_vmethod_named(*using_no_escape_builders, "initializeNullCarModel");

  EXPECT_EQ(4, initialize_null_model->get_dex_code()->get_registers_size());

  // Check there is a register that holds NULL and is passed to
  // the car's model field.
  auto insns = initialize_null_model->get_dex_code()->get_instructions();

  // First instruction should hold the null value.
  EXPECT_EQ(DOPCODE_CONST_4, insns[0]->opcode());
  uint16_t null_reg = insns[0]->dest();

  for (const auto& insn : insns) {
    auto opcode = insn->opcode();
    if (dex_opcode::is_iput(opcode)) {
      DexFieldRef* field =
          static_cast<const DexOpcodeField*>(insn)->get_field();
      if (field->get_class() == car->get_type()) {
        EXPECT_EQ(null_reg, insn->src(0));
      }
    }
  }
}

/*
 * Check builder is actually defined.
 */
TEST_F(PreVerify, RemoveDarBuilder) {
  auto dar = find_class_named(classes, "Lcom/facebook/redex/test/instr/Dar;");
  EXPECT_NE(nullptr, dar);

  auto dar_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Dar$Builder;");
  EXPECT_NE(nullptr, dar_builder);
}

/*
 * Ensure the builder was not removed, and no methods were inlined.
 */
TEST_F(PostVerify, RemoveDarBuilder) {
  auto dar = find_class_named(classes, "Lcom/facebook/redex/test/instr/Dar;");
  EXPECT_NE(nullptr, dar);

  auto dar_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Dar$Builder;");
  EXPECT_NE(nullptr, dar_builder);

  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_dar = find_vmethod_named(*using_no_escape_builders,
                                           "initializeDar_KeepBuilder");
  EXPECT_NE(nullptr, initialize_dar);

  // Build call not inlined.
  EXPECT_NE(nullptr,
            find_invoke(initialize_dar, DOPCODE_INVOKE_VIRTUAL, "build"));
}

TEST_F(PostVerify, RemoveCarBuilder_uninitializedModelInOneCase) {
  auto car = find_class_named(classes, "Lcom/facebook/redex/test/instr/Car;");
  auto car_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Car$Builder;");
  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_null_model = find_vmethod_named(
      *using_no_escape_builders, "initializeNullOrDefinedCarModel");

  // Check there is a register that holds NULL and is passed to
  // the car's model field.
  auto insns = initialize_null_model->get_dex_code()->get_instructions();

  // First instruction should hold the null value, since 'model' can be
  // undefined.
  EXPECT_EQ(DOPCODE_CONST_4, insns[0]->opcode());
  uint16_t different_reg = insns[0]->dest();

  for (const auto& insn : insns) {
    auto opcode = insn->opcode();

    if (opcode == DOPCODE_CONST_STRING) {
      EXPECT_EQ(different_reg, insn->dest());
    }
  }
}

TEST_F(PreVerify, RemoveBPCBuilder) {
  auto bpc = find_class_named(classes, "Lcom/facebook/redex/test/instr/BPC;");
  EXPECT_NE(nullptr, bpc);

  auto bpc_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/BPC$Builder;");
  EXPECT_NE(nullptr, bpc_builder);
}

TEST_F(PostVerify, RemoveBPCBuilder) {
  auto bpc = find_class_named(classes, "Lcom/facebook/redex/test/instr/BPC;");
  EXPECT_NE(nullptr, bpc);

  auto bpc_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/BPC$Builder");
  EXPECT_EQ(nullptr, bpc_builder);

  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_bpc =
      find_vmethod_named(*using_no_escape_builders, "initializeBPC");
  EXPECT_NE(nullptr, initialize_bpc);

  DexType* builder_type =
      DexType::get_type("Lcom/facebook/redex/test/instr/BPC$Builder");
  check_no_builder(initialize_bpc, builder_type);
}
