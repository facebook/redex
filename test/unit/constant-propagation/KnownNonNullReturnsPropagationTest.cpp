/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ConstantPropagationTestUtil.h"
#include "IRAssembler.h"

namespace {

using KnownNonNullReturnsPrimitiveAnalyzer =
    InstructionAnalyzerCombiner<cp::KnownNonNullReturnsAnalyzer,
                                cp::PrimitiveAnalyzer>;

} // namespace

struct KnownNonNullReturnsPropagationTest : public ConstantPropagationTest {
  KnownNonNullReturnsPrimitiveAnalyzer m_analyzer{nullptr, nullptr};
};

// The return value of View.requireViewById is known non-null. A Kotlin null
// check on the result should be eliminated.
TEST_F(KnownNonNullReturnsPropagationTest, RequireViewByIdNullCheckEliminated) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (const v1 42)
      (invoke-virtual (v0 v1) "Landroid/view/View;.requireViewById:(I)Landroid/view/View;")
      (move-result-object v2)
      (invoke-static (v2) "Lkotlin/jvm/internal/Intrinsics;.checkNotNullExpressionValue:(Ljava/lang/Object;Ljava/lang/String;)V")
      (return-object v2)
    )
  )");

  do_const_prop(code.get(), m_analyzer);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (const v1 42)
      (invoke-virtual (v0 v1) "Landroid/view/View;.requireViewById:(I)Landroid/view/View;")
      (move-result-object v2)
      (return-object v2)
    )
  )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

// Without the KnownNonNullReturnsAnalyzer, the null check should NOT be
// eliminated, because the default PrimitiveAnalyzer does not know that
// requireViewById returns non-null.
TEST_F(KnownNonNullReturnsPropagationTest,
       RequireViewByIdNullCheckKeptWithoutAnalyzer) {
  const auto* code_str = R"(
    (
      (load-param-object v0)
      (const v1 42)
      (invoke-virtual (v0 v1) "Landroid/view/View;.requireViewById:(I)Landroid/view/View;")
      (move-result-object v2)
      (invoke-static (v2) "Lkotlin/jvm/internal/Intrinsics;.checkNotNullExpressionValue:(Ljava/lang/Object;Ljava/lang/String;)V")
      (return-object v2)
    )
  )";

  auto code = assembler::ircode_from_string(code_str);
  // Use default ConstantPrimitiveAnalyzer (no KnownNonNullReturnsAnalyzer).
  do_const_prop(code.get(), cp::ConstantPrimitiveAnalyzer());

  auto expected_code = assembler::ircode_from_string(code_str);
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

// A method not in the known-non-null list should not have its null check
// eliminated.
TEST_F(KnownNonNullReturnsPropagationTest, UnknownMethodNullCheckKept) {
  const auto* code_str = R"(
    (
      (load-param-object v0)
      (invoke-virtual (v0) "Landroid/view/View;.getTag:()Ljava/lang/Object;")
      (move-result-object v1)
      (invoke-static (v1) "Lkotlin/jvm/internal/Intrinsics;.checkNotNullExpressionValue:(Ljava/lang/Object;Ljava/lang/String;)V")
      (return-object v1)
    )
  )";

  auto code = assembler::ircode_from_string(code_str);
  do_const_prop(code.get(), m_analyzer);

  auto expected_code = assembler::ircode_from_string(code_str);
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

// Test that NEZ propagates through moves: a null check on a copy of the result
// should also be eliminated.
TEST_F(KnownNonNullReturnsPropagationTest, NezPropagatesThroughMove) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (const v1 42)
      (invoke-virtual (v0 v1) "Landroid/view/View;.requireViewById:(I)Landroid/view/View;")
      (move-result-object v2)
      (move-object v3 v2)
      (invoke-static (v3) "Lkotlin/jvm/internal/Intrinsics;.checkNotNullExpressionValue:(Ljava/lang/Object;Ljava/lang/String;)V")
      (return-object v3)
    )
  )");

  do_const_prop(code.get(), m_analyzer);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (const v1 42)
      (invoke-virtual (v0 v1) "Landroid/view/View;.requireViewById:(I)Landroid/view/View;")
      (move-result-object v2)
      (move-object v3 v2)
      (return-object v3)
    )
  )");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
