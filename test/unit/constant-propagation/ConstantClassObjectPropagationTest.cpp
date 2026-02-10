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

struct ConstantClassObjectPropagationTest : public ConstantPropagationTest {
 public:
  ConstantClassObjectPropagationTest() {
    {
      ClassCreator creator(DexType::make_type("Ljava/lang/Class;"));
      creator.set_super(type::java_lang_Object());
      creator.set_external();

      auto* isInstance =
          DexMethod::make_method(
              "Ljava/lang/Class;.isInstance:(Ljava/lang/Object;)Z")
              ->make_concrete(ACC_PUBLIC, true);
      creator.add_method(isInstance);

      creator.create();
    }

    {
      ClassCreator creator(DexType::make_type("LA;"));
      creator.set_super(type::java_lang_Object());
      auto* a_constructor =
          DexMethod::make_method("LA;.<init>:(LA;)V")
              ->make_concrete(ACC_PUBLIC | ACC_CONSTRUCTOR, true);
      creator.add_method(a_constructor);
      auto* a_class = DexMethod::make_method("LA;.klass:()Ljava/lang/Class;")
                          ->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
      creator.add_method(a_class);
      creator.create();
    }
  }
};

using ConstantClassObjectAnalyzer =
    InstructionAnalyzerCombiner<cp::ConstantClassObjectAnalyzer,
                                cp::PrimitiveAnalyzer>;

TEST_F(ConstantClassObjectPropagationTest, isInstanceConst) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-class "LA;")
      (move-result-pseudo-object v0)
      (new-instance "LA;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LA;.<init>:(LA;)V")
      (invoke-virtual (v0 v1) "Ljava/lang/Class;.isInstance:(Ljava/lang/Object;)Z")
      (move-result v2)
      (return v2)
    )
)");

  // auto state = ConstantClassObjectDomain();
  do_const_prop(code.get(), ConstantClassObjectAnalyzer(nullptr, nullptr));

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-class "LA;")
      (move-result-pseudo-object v0)
      (new-instance "LA;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LA;.<init>:(LA;)V")
      (instance-of v1 "LA;")
      (move-result-pseudo v2)
      (return v2)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(ConstantClassObjectPropagationTest, isInstanceNonConst) {
  const auto* code_str = R"(
    (
      (invoke-static () "LA;.klass:()Ljava/lang/Class;")
      (move-result-object v0)
      (new-instance "LA;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LA;.<init>:(LA;)V")
      (invoke-virtual (v0 v1) "Ljava/lang/Class;.isInstance:(Ljava/lang/Object;)Z")
      (move-result v2)
      (return v2)
    )
)";

  auto code = assembler::ircode_from_string(code_str);

  // auto state = ConstantClassObjectDomain();
  do_const_prop(code.get(), ConstantClassObjectAnalyzer(nullptr, nullptr));

  auto expected_code = assembler::ircode_from_string(code_str);

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
