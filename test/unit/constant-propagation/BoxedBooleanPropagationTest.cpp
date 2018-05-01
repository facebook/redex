/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagation.h"

#include <gtest/gtest.h>

#include "Creators.h"
#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"

struct BoxedBooleanTest : public ConstantPropagationTest {
 public:
  BoxedBooleanTest() {
    ClassCreator creator(DexType::make_type("Ljava/lang/Boolean;"));
    creator.set_super(get_object_type());

    auto boolean_true = static_cast<DexField*>(
        DexField::make_field("Ljava/lang/Boolean;.TRUE:Ljava/lang/Boolean"));
    auto boolean_false = static_cast<DexField*>(
        DexField::make_field("Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean"));
    boolean_true->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
    boolean_false->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
    creator.add_field(boolean_true);
    creator.add_field(boolean_false);

    auto valueof = static_cast<DexMethod*>(DexMethod::make_method(
        "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;"));
    valueof->make_concrete(ACC_PUBLIC, true);
    auto booleanvalue = static_cast<DexMethod*>(
        DexMethod::make_method("Ljava/lang/Boolean;.booleanValue:()Z"));
    valueof->make_concrete(ACC_PUBLIC, true);
    creator.add_method(valueof);
    creator.add_method(valueof);

    creator.create();
  }
};

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

  using Analyzer =
      InstructionSubAnalyzerCombiner<cp::BoxedBooleanSubAnalyzer,
                                     cp::ConstantPrimitiveSubAnalyzer>;

  do_const_prop(code.get(), [analyzer = Analyzer()](auto* insn, auto* env) {
    analyzer.run(insn, env);
  });

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

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
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

  using Analyzer =
      InstructionSubAnalyzerCombiner<cp::BoxedBooleanSubAnalyzer,
                                     cp::ConstantPrimitiveSubAnalyzer>;

  do_const_prop(code.get(), [analyzer = Analyzer()](auto* insn, auto* env) {
    analyzer.run(insn, env);
  });

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v0 0)
      (invoke-static (v0) "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;")
      (move-result v0)
      (invoke-virtual (v0) "Ljava/lang/Boolean;.booleanValue:()Z")
      (move-result v0)
      (goto :if-true-label)
      (const v0 0)
      (:if-true-label)
      (const v0 1)
      (return v0)
    )
)");

  EXPECT_EQ(assembler::to_s_expr(code.get()),
            assembler::to_s_expr(expected_code.get()));
}
