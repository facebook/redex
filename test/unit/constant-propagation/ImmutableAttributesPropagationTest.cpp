/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagation.h"

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
