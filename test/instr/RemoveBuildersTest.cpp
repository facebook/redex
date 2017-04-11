/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
    DexOpcode opcode = insn->opcode();

    if (opcode == OPCODE_NEW_INSTANCE || opcode == OPCODE_INVOKE_DIRECT) {
      DexType* cls_type = static_cast<DexOpcodeType*>(insn)->get_type();
      EXPECT_NE(builder_type, cls_type);
    } else if (is_iget(opcode) || is_iput(opcode)) {
      DexField* field = static_cast<const DexOpcodeField*>(insn)->field();
      EXPECT_NE(builder_type, field->get_class());
    }
  }
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

  // No build call.
  EXPECT_EQ(nullptr,
            find_invoke(initialize_method, OPCODE_INVOKE_DIRECT, "build"));
  EXPECT_EQ(
      nullptr,
      find_invoke(initialize_more_arguments, OPCODE_INVOKE_DIRECT, "build"));

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

  EXPECT_NE(nullptr, initialize_bar_different_regs);
  EXPECT_NE(nullptr, initialize_bar);

  // No build call.
  EXPECT_EQ(nullptr,
            find_invoke(
                initialize_bar_different_regs, OPCODE_INVOKE_DIRECT, "build"));
  EXPECT_EQ(nullptr,
            find_invoke(initialize_bar, OPCODE_INVOKE_DIRECT, "build"));
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

namespace {
  const size_t POST_VERIFY_INITIALIZE_BAR_DIFFERENT_REG = 4;
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

  // While removing the builder 3 moves are added:
  // * 2 for both iputs -> where we move the old value into the new register
  // * 1 for getting the field's value -> where we get the value from the new register
  uint16_t num_dest = 0;
  uint16_t num_src = 0;

  auto insns = initialize_bar_different_regs->get_dex_code()->get_instructions();
  for (const auto& insn : insns) {
    DexOpcode opcode = insn->opcode();

    if (opcode == OPCODE_MOVE) {
      uint16_t src = insn->src(0);
      uint16_t dest = insn->dest();

      if (src == POST_VERIFY_INITIALIZE_BAR_DIFFERENT_REG) {
        num_src++;
      } else if (dest == POST_VERIFY_INITIALIZE_BAR_DIFFERENT_REG) {
        num_dest++;
      }
    }
  }

  EXPECT_EQ(2, num_dest);
  EXPECT_EQ(1, num_src);
}

namespace {

const size_t PRE_VERIFY_INITIALIZE_CAR_REGS = 5;
const size_t POST_VERIFY_INITIALIZE_CAR_PARAM = 5;

} // namespace

/*
 * Check builder is actually defined.
 */
TEST_F(PreVerify, RemoveCarBuilder) {
  auto car = find_class_named(classes, "Lcom/facebook/redex/test/instr/Car;");
  EXPECT_NE(nullptr, car);

  auto car_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Car$Builder;");
  EXPECT_NE(nullptr, car_builder);

  // Check previous number of registers for initialize method.
  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_null_model =
      find_vmethod_named(*using_no_escape_builders, "initializeNullCarModel");
  EXPECT_EQ(PRE_VERIFY_INITIALIZE_CAR_REGS,
            initialize_null_model->get_dex_code()->get_registers_size());
}

/*
 * Ensure the builder was removed and all calls were appropriately removed.
 */
TEST_F(PostVerify, RemoveCarBuilder) {
  auto car = find_class_named(classes, "Lcom/facebook/redex/test/instr/Car;");
  EXPECT_NE(nullptr, car);

  // Check builder class was removed.
  auto car_builder =
      find_class_named(classes, "Lcom/facebook/redex/test/instr/Car$Builder;");
  EXPECT_EQ(nullptr, car_builder);

  auto using_no_escape_builders = find_class_named(
      classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_null_model =
      find_vmethod_named(*using_no_escape_builders, "initializeNullCarModel");

  EXPECT_NE(nullptr, initialize_null_model);
  EXPECT_EQ(PRE_VERIFY_INITIALIZE_CAR_REGS + 1,
            initialize_null_model->get_dex_code()->get_registers_size());

  // Check there is a register that holds NULL and is passed to
  // the car's model field.
  auto insns = initialize_null_model->get_dex_code()->get_instructions();

  // First instruction should hold the null value.
  EXPECT_EQ(OPCODE_CONST_4, insns[0]->opcode());
  uint16_t null_reg = insns[0]->dest();

  // While removing the builders 2 moves are added:
  // * one for the undefined field: move <reg>, null_reg
  // * one for the 'version' field, which uses a method parameter: move <reg>, 5
  std::vector<uint16_t> used_regs;
  std::vector<uint16_t> expected_regs = {null_reg,
                                         POST_VERIFY_INITIALIZE_CAR_PARAM};

  for (const auto& insn : insns) {
    DexOpcode opcode = insn->opcode();

    if (opcode == OPCODE_MOVE) {
      used_regs.emplace_back(insn->src(0));
    }
  }

  std::sort(used_regs.begin(), used_regs.end());
  EXPECT_EQ(expected_regs, used_regs);
}
