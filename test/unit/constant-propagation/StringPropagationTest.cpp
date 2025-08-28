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

    auto* equals = DexMethod::make_method(
                       "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
                       ->make_concrete(ACC_PUBLIC, true);
    auto* hashCode = DexMethod::make_method("Ljava/lang/String;.hashCode:()I")
                         ->make_concrete(ACC_PUBLIC, true);
    creator.add_method(equals);
    creator.add_method(hashCode);

    creator.create();
  }
};

using StringAnalyzer =
    InstructionAnalyzerCombiner<cp::StringAnalyzer, cp::PrimitiveAnalyzer>;
using PackageStringAnalyzer =
    InstructionAnalyzerCombiner<cp::PackageNameAnalyzer,
                                cp::StringAnalyzer,
                                cp::PrimitiveAnalyzer>;

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

  auto state = cp::StringAnalyzerState::get();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr));

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
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_equals()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::get();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

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
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_equals()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::get();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

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
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_hashCode()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::get();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

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

TEST_F(StringTest, package_equals_false) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v2)
      (invoke-virtual (v2) "Landroid/content/ContextWrapper;.getPackageName:()Ljava/lang/String;")
      (move-result-object v2)
      (const-string "nope")
      (move-result-pseudo-object v1)
      (invoke-virtual (v1 v2) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-eqz v0 :zero)
      (const v2 2)
      (return v2)
      (:zero)
      (const v2 1)
      (return v2)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_equals()};
  config.pure_methods = &pure_methods;
  auto state = cp::PackageNameState::get("com.facebook.redextest");
  do_const_prop(code.get(), PackageStringAnalyzer(&state, nullptr, nullptr),
                config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v2)
      (invoke-virtual (v2) "Landroid/content/ContextWrapper;.getPackageName:()Ljava/lang/String;")
      (move-result-object v2)
      (const-string "nope")
      (move-result-pseudo-object v1)
      (invoke-virtual (v1 v2) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (const v0 0)
      (const v2 1)
      (return v2)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, package_equals_true) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v2)
      (invoke-virtual (v2) "Landroid/content/ContextWrapper;.getPackageName:()Ljava/lang/String;")
      (move-result-object v2)
      (const-string "com.facebook.redextest")
      (move-result-pseudo-object v1)
      (invoke-virtual (v1 v2) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (move-result v0)
      (if-eqz v0 :zero)
      (const v2 2)
      (return v2)
      (:zero)
      (const v2 1)
      (return v2)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_equals()};
  config.pure_methods = &pure_methods;
  auto package_state = cp::PackageNameState::get("com.facebook.redextest");
  auto string_state = cp::StringAnalyzerState::get();
  do_const_prop(code.get(),
                PackageStringAnalyzer(&package_state, &string_state, nullptr),
                config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v2)
      (invoke-virtual (v2) "Landroid/content/ContextWrapper;.getPackageName:()Ljava/lang/String;")
      (move-result-object v2)
      (const-string "com.facebook.redextest")
      (move-result-pseudo-object v1)
      (invoke-virtual (v1 v2) "Ljava/lang/String;.equals:(Ljava/lang/Object;)Z")
      (const v0 1)
      (const v2 2)
      (return v2)
    )
)");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
