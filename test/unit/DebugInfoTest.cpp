/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "IRCode.h"
#include "InstructionLowering.h"

TEST(DexPositionTest, multiplePositionBeforeOpcode) {
  g_redex = new RedexContext();

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
      (.pos "LFoo;.bar:()V" "Foo.java" 123)
      (.pos "LFoo;.bar:()V" "Foo.java" 124)
      (const v0 0)
      (return-void)
    )
  )");
  code->set_debug_item(std::make_unique<DexDebugItem>());
  method->set_code(std::move(code));

  instruction_lowering::lower(method);
  method->sync();
  method->balloon();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (.pos "LFoo;.bar:()V" "Foo.java" 124)
      (const v0 0)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(method->get_code()),
            assembler::to_s_expr(expected_code.get()));

  delete g_redex;
}

TEST(DexPositionTest, consecutiveIdenticalPositions) {
  g_redex = new RedexContext();

  auto method =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.bar:()V"));
  method->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

  auto code = assembler::ircode_from_string(R"(
    (
      (.pos "LFoo;.bar:()V" "Foo.java" 123)
      (const v0 0)
      (.pos "LFoo;.bar:()V" "Foo.java" 123)
      (const v0 0)
      (return-void)
    )
  )");
  code->set_debug_item(std::make_unique<DexDebugItem>());
  method->set_code(std::move(code));

  instruction_lowering::lower(method);
  method->sync();
  method->balloon();

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (.pos "LFoo;.bar:()V" "Foo.java" 123)
      (const v0 0)
      (const v0 0)
      (return-void)
    )
  )");

  EXPECT_EQ(assembler::to_s_expr(method->get_code()),
            assembler::to_s_expr(expected_code.get()));

  delete g_redex;
}
