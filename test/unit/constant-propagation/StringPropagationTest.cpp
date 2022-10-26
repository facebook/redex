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

struct StringTest : public ConstantPropagationTest {
 public:
  StringTest() {
    ClassCreator creator(DexType::make_type("Ljava/lang/String;"));
    creator.set_super(type::java_lang_Object());
    creator.set_external();

    auto equals = DexMethod::make_method(
                      "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
                      ->make_concrete(ACC_PUBLIC, true);
    auto hashCode = DexMethod::make_method("Ljava/lang/String;.hashCode:()I")
                        ->make_concrete(ACC_PUBLIC, true);
    creator.add_method(equals);
    creator.add_method(hashCode);

    creator.create();
  }
};

using StringAnalyzer =
    InstructionAnalyzerCombiner<cp::StringAnalyzer, cp::PrimitiveAnalyzer>;

TEST_F(StringTest, neq) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "A")
      (move-result-pseudo-object v0)
      (const-string "B")
      (move-result-pseudo-object v1)
      (if-ne v0 v1 :exit)
      (move-object v0 v1)
      (:exit)
      (return v0)
    )
)");

  do_const_prop(code.get(), StringAnalyzer());

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "A")
      (move-result-pseudo-object v0)
      (const-string "B")
      (move-result-pseudo-object v1)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, equals_false) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "A")
      (move-result-pseudo-object v0)
      (const-string "B")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (return v0)
    )
)");

  cp::Transform::Config config;
  config.replace_move_result_with_consts = true;
  do_const_prop(code.get(), StringAnalyzer(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "A")
      (move-result-pseudo-object v0)
      (const-string "B")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (const v0 0)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, equals_true) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "A")
      (move-result-pseudo-object v0)
      (const-string "A")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (return v0)
    )
)");

  cp::Transform::Config config;
  config.replace_move_result_with_consts = true;
  do_const_prop(code.get(), StringAnalyzer(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "A")
      (move-result-pseudo-object v0)
      (const-string "A")
      (move-result-pseudo-object v1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (const v0 1)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, hashCode) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "A")
      (move-result-pseudo-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.hashCode:()I")
      (move-result v0)
      (return v0)
    )
)");

  cp::Transform::Config config;
  config.replace_move_result_with_consts = true;
  do_const_prop(code.get(), StringAnalyzer(), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "A")
      (move-result-pseudo-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.hashCode:()I")
      (const v0 65)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
