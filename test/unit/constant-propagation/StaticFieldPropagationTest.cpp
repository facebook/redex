/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationAnalysis.h"

#include <gtest/gtest.h>

#include "ConstantPropagationTestUtil.h"
#include "Creators.h"
#include "IRAssembler.h"

struct StaticFieldTest : public ConstantPropagationTest {
 public:
  StaticFieldTest() {
    // Test 1 -- final field
    ClassCreator creator1(DexType::make_type("Lcom/facebook/R$bool;"));
    creator1.set_super(type::java_lang_Object());

    auto boolean_should_log_I =
        DexField::make_field("Lcom/facebook/R$bool;.should_log:I")
            ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
    boolean_should_log_I->get_static_value()->value(123);
    creator1.add_field(boolean_should_log_I);

    creator1.create();

    // Test 2 -- not final field
    ClassCreator creator2(DexType::make_type("Lcom/facebook/R$integer;"));
    creator2.set_super(type::java_lang_Object());

    auto integer_loop_count =
        DexField::make_field("Lcom/facebook/R$integer;.loop_count:I")
            ->make_concrete(ACC_PUBLIC | ACC_STATIC);
    creator2.add_field(integer_loop_count);

    creator2.create();

    assembler::class_from_string(R"(
    (class (public final) "LAnother;"
      (field (public static) "LAnother;.a:J")
      (field (public static final) "LAnother;.b:J" "80000000")
    )
  )");
  }
};

using StaticFieldAnalyzer =
    InstructionAnalyzerCombiner<cp::StaticFinalFieldAnalyzer,
                                cp::HeapEscapeAnalyzer,
                                cp::PrimitiveAnalyzer>;

/*
 * This test should be optimized, and remove the if-statement, since we know
 * should log is much greater than 0, and as such the if-statement is not
 * necessary, and should_log is final!
 */
TEST_F(StaticFieldTest, FinalLessThan) {
  auto code = assembler::ircode_from_string(R"(
    (
     (sget "Lcom/facebook/R$bool;.should_log:I")
     (move-result-pseudo v0)
     (if-ltz v0 :if-true-label)
     (const v0 1)
     (return v0)
     (:if-true-label)
     (new-instance "Ljava/lang/RuntimeException;")
     (move-result-pseudo-object v0)
     (const-string "FinalLessThan")
     (move-result-pseudo-object v1)
     (invoke-direct (v0 v1) "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V")
     (throw v0)
    )
)");

  do_const_prop(code.get(), StaticFieldAnalyzer());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const v0 123)
     (const v0 1)
     (return v0)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

/*
 * This test should not be optimized, since loop_count is not final, and
 * therefore we cannot be sure that it is greater than 0.
 */
TEST_F(StaticFieldTest, NotFinalLessThan) {
  auto code_expression = R"(
  (
    (sget "Lcom/facebook/R$integer;.loop_count:I")
    (move-result-pseudo v0)
    (if-ltz v0 :if-true-label)
    (const v0 1)
    (return v0)
    (:if-true-label)
    (new-instance "Ljava/lang/RuntimeException;")
    (move-result-pseudo-object v0)
    (const-string "NotFinalLessThan")
    (move-result-pseudo-object v1)
    (invoke-direct (v0 v1) "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V")
    (throw v0)
  )
)";
  auto code = assembler::ircode_from_string(code_expression);
  auto expected_code = assembler::ircode_from_string(code_expression);

  do_const_prop(code.get(), StaticFieldAnalyzer());
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StaticFieldTest, WideFinals) {
  auto code = assembler::ircode_from_string(R"(
    (
     (sget-wide "LAnother;.b:J")
     (move-result-pseudo-wide v0)
     (if-ltz v0 :if-true-label)
     (const v0 1)
     (return v0)
     (:if-true-label)
     (new-instance "Ljava/lang/RuntimeException;")
     (move-result-pseudo-object v0)
     (const-string "Oh no")
     (move-result-pseudo-object v1)
     (invoke-direct (v0 v1) "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V")
     (throw v0)
    )
)");

  do_const_prop(code.get(), StaticFieldAnalyzer());

  auto expected_code = assembler::ircode_from_string(R"(
    (
     (const-wide v0 2147483648)
     (const v0 1)
     (return v0)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
