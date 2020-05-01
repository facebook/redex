/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include <gtest/gtest.h>

#include "ConstantPropagationTestUtil.h"
#include "ConstructorParams.h"
#include "Creators.h"
#include "IRAssembler.h"
#include "JarLoader.h"

using ImmutableAnalyzer =
    InstructionAnalyzerCombiner<cp::StringAnalyzer,
                                cp::ImmutableAttributeAnalyzer,
                                cp::PrimitiveAnalyzer>;

struct ImmutableTest : public ConstantPropagationTest {
 public:
  ImmutableTest() {
    always_assert(load_class_file(std::getenv("enum_class_file")));
    m_config.replace_move_result_with_consts = true;
    auto integer_valueOf = method::java_lang_Integer_valueOf();
    auto integer_intValue = method::java_lang_Integer_intValue();
    // The intValue of integer is initialized through the static invocation.
    m_immut_analyzer_state.add_initializer(integer_valueOf, integer_intValue)
        .set_src_id_of_attr(0)
        .set_obj_to_dest();
    m_analyzer = ImmutableAnalyzer(nullptr, &m_immut_analyzer_state, nullptr);
  }

  cp::ImmutableAttributeAnalyzerState m_immut_analyzer_state;
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

TEST_F(ImmutableTest, integer_meets) {
  std::string code_str = R"(
    (
      (load-param v2)
      (load-param v3)

      (if-nez v2 :if-true-label)
      (const v1 100)
      (invoke-static (v1) "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;")
      (move-result v0)
      (goto :end)

      (:if-true-label)
      (invoke-static (v2) "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;")
      (move-result v0)

      (:end)
      (invoke-virtual (v0) "Ljava/lang/Integer;.intValue:()I")
      (move-result v0)
    )
  )";
  auto code = assembler::ircode_from_string(code_str);

  do_const_prop(code.get(), m_analyzer, m_config);
  auto expected_code = assembler::ircode_from_string(code_str);
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
                ImmutableAnalyzer(nullptr, &analyzer_state, nullptr),
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

TEST_F(ImmutableTest, enum_constructor) {
  auto method = assembler::method_from_string(R"(
    (method (private constructor) "LFoo;.<init>:(Ljava/lang/String;I)V"
    (
      (load-param-object v0)
      (load-param-object v1)
      (load-param v2)
      (invoke-direct (v0 v1 v2) "Ljava/lang/Enum;.<init>:(Ljava/lang/String;I)V")
      (return-void)
    )
    )
  )");
  method->get_code()->build_cfg(false);
  auto creator = ClassCreator(method->get_class());
  creator.set_super(type::java_lang_Enum());
  creator.set_access(ACC_PUBLIC | ACC_ENUM);
  creator.add_method(method);
  auto foo_cls = creator.create();
  cp::ImmutableAttributeAnalyzerState analyzer_state;
  cp::immutable_state::analyze_constructors({foo_cls}, &analyzer_state);
  EXPECT_EQ(analyzer_state.method_initializers.count(method), 1);

  // Enum immutable attributes 'name' and 'ordinal' can be propagated.
  auto code = assembler::ircode_from_string(R"(
  (
    (const v0 0)
    (const-string "A")
    (move-result-pseudo-object v1)
    (new-instance "LFoo;")
    (move-result-pseudo-object v2)
    (invoke-direct (v2 v1 v0) "LFoo;.<init>:(Ljava/lang/String;I)V")
    (invoke-virtual (v2) "LFoo;.name:()Ljava/lang/String;")
    (move-result-object v3)
    (invoke-virtual (v2) "LFoo;.ordinal:()I")
    (move-result-object v4)
  )
  )");
  do_const_prop(code.get(),
                ImmutableAnalyzer(nullptr, &analyzer_state, nullptr),
                m_config);

  auto expected_code = assembler::ircode_from_string(R"(
  (
    (const v0 0)
    (const-string "A")
    (move-result-pseudo-object v1)
    (new-instance "LFoo;")
    (move-result-pseudo-object v2)
    (invoke-direct (v2 v1 v0) "LFoo;.<init>:(Ljava/lang/String;I)V")
    (invoke-virtual (v2) "LFoo;.name:()Ljava/lang/String;")
    (const-string "A")
    (move-result-pseudo-object v3)
    (invoke-virtual (v2) "LFoo;.ordinal:()I")
    (const v4 0)
  )
  )");
  EXPECT_CODE_EQ(code.get(), expected_code.get());
}
