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
#include "IRInstruction.h"

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
    auto* isEmpty = DexMethod::make_method("Ljava/lang/String;.isEmpty:()Z")
                        ->make_concrete(ACC_PUBLIC, true);
    auto* charAt = DexMethod::make_method("Ljava/lang/String;.charAt:(I)C")
                       ->make_concrete(ACC_PUBLIC, true);
    creator.add_method(equals);
    creator.add_method(hashCode);
    creator.add_method(isEmpty);
    creator.add_method(charAt);

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

  auto state = cp::StringAnalyzerState::make_default();
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
  auto state = cp::StringAnalyzerState::make_default();
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
  auto state = cp::StringAnalyzerState::make_default();
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
  auto state = cp::StringAnalyzerState::make_default();
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
  auto state =
      cp::PackageNameState::make(std::string("com.facebook.redextest"));
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
  auto package_state =
      cp::PackageNameState::make(std::string("com.facebook.redextest"));
  auto string_state = cp::StringAnalyzerState::make_default();
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

TEST_F(StringTest, isEmpty_true) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "")
      (move-result-pseudo-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.isEmpty:()Z")
      (move-result v0)
      (return v0)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_isEmpty()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::make_default();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "")
      (move-result-pseudo-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.isEmpty:()Z")
      (const v0 1)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, isEmpty_false) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.isEmpty:()Z")
      (move-result v0)
      (return v0)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_isEmpty()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::make_default();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.isEmpty:()Z")
      (const v0 0)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, isEmpty_unknown_receiver) {
  auto code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.isEmpty:()Z")
      (move-result v0)
      (return v0)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_isEmpty()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::make_default();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (load-param-object v0)
      (invoke-virtual (v0) "Ljava/lang/String;.isEmpty:()Z")
      (move-result v0)
      (return v0)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, charAt_ascii) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (const v1 1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_charAt()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::make_default();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (const v1 1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (const v2 98)
      (return v2)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, charAt_out_of_bounds) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (const v1 5)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_charAt()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::make_default();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

  // Out-of-bounds: must not fold, preserve runtime exception
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (const v1 5)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, charAt_negative_index) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (const v1 -1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_charAt()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::make_default();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

  // Negative index: must not fold, preserve runtime exception
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (const v1 -1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, charAt_non_ascii) {
  // "é" (U+00E9) is 2 bytes in MUTF-8 but 1 UTF-16 code unit.
  // DexString::is_simple() returns false, so charAt must not fold.
  const auto* non_ascii = DexString::make_string("\xc3\xa9");
  ASSERT_FALSE(non_ascii->is_simple());

  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "placeholder")
      (move-result-pseudo-object v0)
      (const v1 0)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  // Replace the placeholder with the non-ASCII string.
  for (auto& mie : InstructionIterable(code.get())) {
    if (mie.insn->opcode() == OPCODE_CONST_STRING) {
      mie.insn->set_string(non_ascii);
      break;
    }
  }

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_charAt()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::make_default();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

  // Non-ASCII: charAt must NOT be folded.
  bool has_move_result = false;
  for (const auto& mie : InstructionIterable(code.get())) {
    if (mie.insn->opcode() == OPCODE_MOVE_RESULT) {
      has_move_result = true;
      break;
    }
  }
  EXPECT_TRUE(has_move_result);
}

TEST_F(StringTest, charAt_unknown_index) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (load-param v1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_charAt()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::make_default();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

  // Unknown index: must not fold
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "abc")
      (move-result-pseudo-object v0)
      (load-param v1)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}

TEST_F(StringTest, charAt_empty_string) {
  auto code = assembler::ircode_from_string(R"(
    (
      (const-string "")
      (move-result-pseudo-object v0)
      (const v1 0)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  cp::Transform::Config config;
  UnorderedSet<DexMethodRef*> pure_methods{method::java_lang_String_charAt()};
  config.pure_methods = &pure_methods;
  auto state = cp::StringAnalyzerState::make_default();
  do_const_prop(code.get(), StringAnalyzer(&state, nullptr), config);

  // Empty string: index 0 is out of bounds, must not fold
  auto expected_code = assembler::ircode_from_string(R"(
    (
      (const-string "")
      (move-result-pseudo-object v0)
      (const v1 0)
      (invoke-virtual (v0 v1) "Ljava/lang/String;.charAt:(I)C")
      (move-result v2)
      (return v2)
    )
)");

  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
