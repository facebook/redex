/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "ConstantPropagationTestUtil.h"
#include "Creators.h"
#include "IRAssembler.h"

struct NullPropagationTest : public ConstantPropagationTest {
 public:
  NullPropagationTest() {
    ClassCreator creator(DexType::make_type("Ljava/lang/Boolean;"));
    creator.set_super(type::java_lang_Object());
    creator.set_external();

    auto* booleanvalue =
        DexMethod::make_method("Ljava/lang/Boolean;.booleanValue:()Z")
            ->make_concrete(ACC_PUBLIC, true);
    creator.add_method(booleanvalue);

    creator.create();
  }
};

// Simplify null-dereference attempts to use throw instruction
TEST_F(NullPropagationTest, NullMonitorEnter) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (monitor-enter v0)
    )
)");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const-string "monitor-enter")
      (move-result-pseudo-object v1)
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v2)
      (invoke-direct (v2 v1) "Ljava/lang/NullPointerException;.<init>:(Ljava/lang/String;)V")
      (throw v2)
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// Simplify null-dereference attempts to use throw instruction
TEST_F(NullPropagationTest, NullInvokeVirtual) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (invoke-virtual (v0) "Ljava/lang/Boolean;.booleanValue:()Z")
      (move-result v0)
    )
)");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const-string "booleanValue")
      (move-result-pseudo-object v1)
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v2)
      (invoke-direct (v2 v1) "Ljava/lang/NullPointerException;.<init>:(Ljava/lang/String;)V")
      (throw v2)
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}

// Simplify null-dereference attempts to use throw instruction
TEST_F(NullPropagationTest, AputInvokeVirtual) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (const v2 0)
      (aput v0 v1 v2)
    )
)");
  do_const_prop(code.get());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (const v1 0)
      (const v2 0)
      (const-string "array access")
      (move-result-pseudo-object v3)
      (new-instance "Ljava/lang/NullPointerException;")
      (move-result-pseudo-object v4)
      (invoke-direct (v4 v3) "Ljava/lang/NullPointerException;.<init>:(Ljava/lang/String;)V")
      (throw v4)
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}
