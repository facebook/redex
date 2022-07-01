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

struct BoxedBooleanTest : public ConstantPropagationTest {
 public:
  BoxedBooleanTest() {
    ClassCreator creator(DexType::make_type("Ljava/lang/Boolean;"));
    creator.set_super(type::java_lang_Object());
    creator.set_external();

    auto boolean_true =
        DexField::make_field("Ljava/lang/Boolean;.TRUE:Ljava/lang/Boolean")
            ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
    auto boolean_false =
        DexField::make_field("Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean")
            ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
    creator.add_field(boolean_true);
    creator.add_field(boolean_false);

    auto valueof = DexMethod::make_method(
                       "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;")
                       ->make_concrete(ACC_PUBLIC, true);
    auto booleanvalue =
        DexMethod::make_method("Ljava/lang/Boolean;.booleanValue:()Z")
            ->make_concrete(ACC_PUBLIC, true);
    creator.add_method(valueof);
    creator.add_method(valueof);

    creator.create();
  }
};

using BoxedBooleanAnalyzer =
    InstructionAnalyzerCombiner<cp::BoxedBooleanAnalyzer,
                                cp::PrimitiveAnalyzer>;

TEST_F(BoxedBooleanTest, booleanValue) {
  auto code = assembler::ircode_from_string(R"(
    (
      (sget-object "Ljava/lang/Boolean;.TRUE:I")
      (move-result-pseudo-object v0)
      (invoke-virtual (v0) "Ljava/lang/Boolean;.booleanValue:()Z")
      (move-result v0)
      (if-eqz v0 :if-true-label)
      (const v0 0)
      (:if-true-label)
      (const v0 1)
      (return v0)
    )
)");

  do_const_prop(code.get(), BoxedBooleanAnalyzer());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (sget-object "Ljava/lang/Boolean;.TRUE:I")
      (move-result-pseudo-object v0)
      (invoke-virtual (v0) "Ljava/lang/Boolean;.booleanValue:()Z")
      (move-result v0)
      (if-eqz v0 :if-true-label)
      (const v0 0)
      (:if-true-label)
      (const v0 1)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(BoxedBooleanTest, valueOf) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (invoke-static (v0) "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;")
      (move-result v0)
      (invoke-virtual (v0) "Ljava/lang/Boolean;.booleanValue:()Z")
      (move-result v0)
      (if-eqz v0 :if-true-label)
      (const v0 0)
      (:if-true-label)
      (const v0 1)
      (return v0)
    )
)");

  do_const_prop(code.get(), BoxedBooleanAnalyzer());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (invoke-static (v0) "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;")
      (move-result v0)
      (invoke-virtual (v0) "Ljava/lang/Boolean;.booleanValue:()Z")
      (move-result v0)
      (const v0 1)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
