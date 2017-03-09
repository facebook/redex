/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <memory>
#include <gtest/gtest.h>
#include <string>

#include "DexInstruction.h"
#include "Match.h"
#include "VerifyUtil.h"

/*
 * Check builder is actually defined.
 */
TEST_F(PreVerify, RemoveFooBuilder) {
  auto foo = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/Foo;");
  EXPECT_NE(nullptr, foo);

  auto foo_builder = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/Foo$Builder;");
  EXPECT_NE(nullptr, foo_builder);
}

/*
 * Ensure the builder was removed and all calls were appropriately
 * replaced / removed.
 */
TEST_F(PostVerify, RemoveFooBuilder) {
  auto foo = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/Foo;");
  EXPECT_NE(nullptr, foo);

  // Check builder class was removed.
  auto foo_builder = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/Foo$Builder;");
  EXPECT_EQ(nullptr, foo_builder);

  auto using_foo = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_method = find_vmethod_named(*using_foo, "initializeFoo");
  EXPECT_NE(nullptr, initialize_method);

  // No build call.
  EXPECT_EQ(nullptr,
            find_invoke(initialize_method, OPCODE_INVOKE_DIRECT, "build"));

  DexType* builder_type = DexType::get_type("Lcom/facebook/redex/test/instr/Foo$Builder;");
  auto insns = initialize_method->get_code()->get_instructions();
  for (const auto& insn : insns) {
    DexOpcode opcode = insn->opcode();

    if (opcode == OPCODE_NEW_INSTANCE) {
      DexType* cls_type = static_cast<DexOpcodeType*>(insn)->get_type();
      EXPECT_NE(builder_type, cls_type);
    } else if (is_iget(opcode) || is_iput(opcode)) {
      DexField* field = static_cast<const DexOpcodeField*>(insn)->field();
      EXPECT_NE(builder_type, field->get_class());
    }
  }
}

/*
 * Check builder is actually defined.
 */
TEST_F(PreVerify, RemoveFooBuilderWithMoreArguments) {
  auto foo = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/FooMoreArguments;");
  EXPECT_NE(nullptr, foo);

  auto foo_builder = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/FooMoreArguments$Builder;");
  EXPECT_NE(nullptr, foo_builder);
}

/*
 * Ensure the builder was not removed and all calls were appropriately
 * replaced / removed / kept.
 *
 * TODO(emmasevastian): will update this after we treat the case where
 *                      we should reallocate registers.
 */
TEST_F(PostVerify, RemoveFooBuilderWithMoreArguments) {
  auto foo = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/FooMoreArguments;");
  EXPECT_NE(nullptr, foo);

  // Check builder class was not removed.
  auto foo_builder = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/FooMoreArguments$Builder;");
  EXPECT_NE(nullptr, foo_builder);

  auto using_foo = find_class_named(
    classes, "Lcom/facebook/redex/test/instr/UsingNoEscapeBuilder;");
  auto initialize_method = find_vmethod_named(*using_foo, "initializeFooWithMoreArguments");
  EXPECT_NE(nullptr, initialize_method);

  // No build call.
  EXPECT_EQ(nullptr,
            find_invoke(initialize_method, OPCODE_INVOKE_DIRECT, "build"));

  DexType* builder_type =
    DexType::get_type("Lcom/facebook/redex/test/instr/FooMoreArguments$Builder;");
  int builder_instances = 0;

  auto insns = initialize_method->get_code()->get_instructions();
  for (const auto& insn : insns) {
    DexOpcode opcode = insn->opcode();

    if (opcode == OPCODE_NEW_INSTANCE) {
      DexType* cls_type = static_cast<DexOpcodeType*>(insn)->get_type();
      if (builder_type == cls_type) {
        builder_instances++;
      }
    }
  }
  EXPECT_EQ(1, builder_instances);
}
