/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "IRAssembler.h"
#include "IRCode.h"
#include "InstructionLowering.h"
#include "RedexTest.h"

class DexPositionTest : public RedexTest {};

TEST_F(DexPositionTest, multiplePositionBeforeOpcode) {
  auto method = DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

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

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}

TEST_F(DexPositionTest, consecutiveIdenticalPositions) {
  auto method = DexMethod::make_method("LFoo;.bar:()V")
                    ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);

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

  EXPECT_CODE_EQ(method->get_code(), expected_code.get());
}
