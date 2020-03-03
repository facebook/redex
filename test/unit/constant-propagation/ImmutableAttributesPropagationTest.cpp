/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "ConstantPropagationTestUtil.h"
#include "Creators.h"
#include "IRAssembler.h"

using ImmutableAnalyzer =
    InstructionAnalyzerCombiner<cp::StringAnalyzer,
                                cp::ImmutableAttributeAnalyzer,
                                cp::PrimitiveAnalyzer>;

struct ImmutableTest : public ConstantPropagationTest {
 public:
  ImmutableTest() {
    m_config.replace_move_result_with_consts = true;
    m_analyzer = ImmutableAnalyzer(
        nullptr, cp::ImmutableAttributeAnalyzerState(), nullptr);
  }

  ImmutableAnalyzer m_analyzer;
  cp::Transform::Config m_config;
};

TEST_F(ImmutableTest, integer) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v1 100)
      (invoke-static (v1) "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;")
      (move-result v0)
      (invoke-virtual (v0) "Ljava/lang/Integer;.intValue:()I")
      (move-result v0)
    )
  )");

  do_const_prop(code.get(), m_analyzer, m_config);
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v1 100)
      (invoke-static (v1) "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;")
      (move-result v0)
      (invoke-virtual (v0) "Ljava/lang/Integer;.intValue:()I")
      (const v0 100)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

/**
 * Java class `Data` has two immutable fields, one is non-private field `id`,
 * another one is a hidden field and we visit it through a function call.
 */
TEST_F(ImmutableTest, object) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const v1 100)
      (const-string "ValueA")
      (move-result-pseudo-object v2)
      (new-instance "LData;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v2 v1) "LData;.<init>:(Ljava/lang/String;I)V")
      (iget v0 "LData;.id:I")
      (move-result-pseudo-object v3)
      (invoke-virtual (v0) "LData;.toString:()Ljava/lang/String;")
      (move-result v4)
    )
  )");

  cp::ImmutableAttributeAnalyzerState analyzer_state;
  {
    // Add initializer for Data
    auto constructor = static_cast<DexMethod*>(
        DexMethod::make_method("LData;.<init>:(Ljava/lang/String;I)V"));
    auto int_field =
        static_cast<DexField*>(DexField::make_field("LData;.id:I"));
    // Assume we do not know the implementation of this method but we know that
    // the method always returns a hidden immutable field.
    auto method_ref =
        DexMethod::make_method("LData;.toString:()Ljava/lang/String;");
    always_assert(!method_ref->is_def() &&
                  !resolve_method(method_ref, MethodSearch::Virtual));
    auto string_getter = static_cast<DexMethod*>(method_ref);
    analyzer_state.add_initializer(constructor, int_field)
        .set_src_id_of_attr(2)
        .set_src_id_of_obj(0);
    analyzer_state.add_initializer(constructor, string_getter)
        .set_src_id_of_attr(1)
        .set_src_id_of_obj(0);
  }
  do_const_prop(code.get(),
                ImmutableAnalyzer(nullptr, analyzer_state, nullptr),
                m_config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const v1 100)
      (const-string "ValueA")
      (move-result-pseudo-object v2)
      (new-instance "LData;")
      (move-result-pseudo-object v0)
      (invoke-direct (v0 v2 v1) "LData;.<init>:(Ljava/lang/String;I)V")
      (const v3 100)
      (invoke-virtual (v0) "LData;.toString:()Ljava/lang/String;")
      (const-string "ValueA")
      (move-result-pseudo-object v4)
    )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
