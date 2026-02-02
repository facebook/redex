/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "KotlinLambdaAnalyzer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "Creators.h"
#include "IRCode.h"
#include "RedexTest.h"

using ::testing::IsNull;
using ::testing::NotNull;

class KotlinLambdaAnalyzerTest : public RedexTest {
 protected:
  // Helper to create an ill-formed Kotlin lambda class without an invoke
  // method.
  static DexClass* create_lambda_without_invoke() {
    static unsigned counter = 0;
    const auto type_name =
        "LLambdaAnalyzerWithoutInvoke$" + std::to_string(counter++) + ";";
    auto* lambda_type = DexType::make_type(type_name);
    auto* kotlin_function_type =
        DexType::make_type("Lkotlin/jvm/functions/Function0;");

    ClassCreator creator(lambda_type);
    creator.set_super(type::kotlin_jvm_internal_Lambda());
    creator.add_interface(kotlin_function_type);
    return creator.create();
  }

  // Helper to create an ill-formed Kotlin lambda class with multiple invoke
  // methods.
  static DexClass* create_lambda_with_multiple_invokes() {
    static unsigned counter = 0;
    const auto type_name =
        "LLambdaAnalyzerMultipleInvokes$" + std::to_string(counter++) + ";";
    auto* lambda_type = DexType::make_type(type_name);
    auto* kotlin_function_type =
        DexType::make_type("Lkotlin/jvm/functions/Function1;");

    ClassCreator creator(lambda_type);
    creator.set_super(type::kotlin_jvm_internal_Lambda());
    creator.add_interface(kotlin_function_type);

    // Add first invoke method
    auto* invoke_proto1 = DexProto::make_proto(
        type::java_lang_Object(),
        DexTypeList::make_type_list({type::java_lang_Object()}));
    auto* invoke_method1 =
        DexMethod::make_method(lambda_type, DexString::make_string("invoke"),
                               invoke_proto1)
            ->make_concrete(ACC_PUBLIC, true);
    auto code1 = std::make_unique<IRCode>(invoke_method1, 2);
    code1->push_back(new IRInstruction(OPCODE_RETURN_OBJECT));
    invoke_method1->set_code(std::move(code1));
    creator.add_method(invoke_method1);

    // Add second invoke method with different signature
    auto* invoke_proto2 = DexProto::make_proto(
        type::java_lang_Object(),
        DexTypeList::make_type_list(
            {type::java_lang_Object(), type::java_lang_Object()}));
    auto* invoke_method2 =
        DexMethod::make_method(lambda_type, DexString::make_string("invoke"),
                               invoke_proto2)
            ->make_concrete(ACC_PUBLIC, true);
    auto code2 = std::make_unique<IRCode>(invoke_method2, 3);
    code2->push_back(new IRInstruction(OPCODE_RETURN_OBJECT));
    invoke_method2->set_code(std::move(code2));
    creator.add_method(invoke_method2);

    return creator.create();
  }

  // Helper to create an ill-formed Kotlin lambda class with a non-public invoke
  // method.
  static DexClass* create_lambda_with_non_public_invoke() {
    static unsigned counter = 0;
    const auto type_name =
        "LLambdaAnalyzerNonPublicInvoke$" + std::to_string(counter++) + ";";
    auto* lambda_type = DexType::make_type(type_name);
    auto* kotlin_function_type =
        DexType::make_type("Lkotlin/jvm/functions/Function0;");

    ClassCreator creator(lambda_type);
    creator.set_super(type::kotlin_jvm_internal_Lambda());
    creator.add_interface(kotlin_function_type);

    // Add a non-public invoke method (private)
    auto* invoke_proto = DexProto::make_proto(type::java_lang_Object(),
                                              DexTypeList::make_type_list({}));
    auto* invoke_method =
        DexMethod::make_method(lambda_type, DexString::make_string("invoke"),
                               invoke_proto)
            ->make_concrete(ACC_PRIVATE, true);
    auto code = std::make_unique<IRCode>(invoke_method, 1);
    code->push_back(new IRInstruction(OPCODE_RETURN_OBJECT));
    invoke_method->set_code(std::move(code));
    creator.add_method(invoke_method);

    return creator.create();
  }

  // Helper to create an ill-formed Kotlin lambda class with a synthetic invoke
  // method.
  static DexClass* create_lambda_with_synthetic_invoke() {
    static unsigned counter = 0;
    const auto type_name =
        "LLambdaAnalyzerSyntheticInvoke$" + std::to_string(counter++) + ";";
    auto* lambda_type = DexType::make_type(type_name);
    auto* kotlin_function_type =
        DexType::make_type("Lkotlin/jvm/functions/Function0;");

    ClassCreator creator(lambda_type);
    creator.set_super(type::kotlin_jvm_internal_Lambda());
    creator.add_interface(kotlin_function_type);

    // Add a synthetic invoke method
    auto* invoke_proto = DexProto::make_proto(type::java_lang_Object(),
                                              DexTypeList::make_type_list({}));
    auto* invoke_method =
        DexMethod::make_method(lambda_type, DexString::make_string("invoke"),
                               invoke_proto)
            ->make_concrete(ACC_PUBLIC | ACC_SYNTHETIC, true);
    auto code = std::make_unique<IRCode>(invoke_method, 1);
    code->push_back(new IRInstruction(OPCODE_RETURN_OBJECT));
    invoke_method->set_code(std::move(code));
    creator.add_method(invoke_method);

    return creator.create();
  }

  // Helper to create a well-formed Kotlin lambda class with a proper invoke
  // method.
  static DexClass* create_lambda_with_invoke() {
    static unsigned counter = 0;
    const auto type_name =
        "LLambdaAnalyzerWithInvoke$" + std::to_string(counter++) + ";";
    auto* lambda_type = DexType::make_type(type_name);
    auto* kotlin_function_type =
        DexType::make_type("Lkotlin/jvm/functions/Function0;");

    ClassCreator creator(lambda_type);
    creator.set_super(type::kotlin_jvm_internal_Lambda());
    creator.add_interface(kotlin_function_type);

    auto* invoke_proto = DexProto::make_proto(type::java_lang_Object(),
                                              DexTypeList::make_type_list({}));
    auto* invoke_method =
        DexMethod::make_method(lambda_type, DexString::make_string("invoke"),
                               invoke_proto)
            ->make_concrete(ACC_PUBLIC, true);
    auto code = std::make_unique<IRCode>(invoke_method, 1);
    code->push_back(new IRInstruction(OPCODE_RETURN_OBJECT));
    invoke_method->set_code(std::move(code));
    creator.add_method(invoke_method);

    return creator.create();
  }
};

TEST_F(KotlinLambdaAnalyzerTest, IsTrivial_RespectsMaxInstructions) {
  auto* lambda_type = DexType::make_type("LAnalyzerTestNonTrivial$1;");
  auto* kotlin_function_type =
      DexType::make_type("Lkotlin/jvm/functions/Function0;");

  ClassCreator creator(lambda_type);
  creator.set_super(type::kotlin_jvm_internal_Lambda());
  creator.add_interface(kotlin_function_type);

  // Create a lambda with 5 instructions
  auto* invoke_proto = DexProto::make_proto(type::java_lang_Object(),
                                            DexTypeList::make_type_list({}));
  auto* invoke_method =
      DexMethod::make_method(lambda_type, DexString::make_string("invoke"),
                             invoke_proto)
          ->make_concrete(ACC_PUBLIC, true);
  auto code = std::make_unique<IRCode>(invoke_method, 1);
  code->push_back(new IRInstruction(OPCODE_CONST));
  code->push_back(new IRInstruction(OPCODE_CONST));
  code->push_back(new IRInstruction(OPCODE_CONST));
  code->push_back(new IRInstruction(OPCODE_CONST));
  code->push_back(new IRInstruction(OPCODE_RETURN_OBJECT));
  invoke_method->set_code(std::move(code));
  creator.add_method(invoke_method);

  const auto* lambda_class = creator.create();
  auto analyzer = KotlinLambdaAnalyzer::analyze(lambda_class);
  ASSERT_TRUE(analyzer.has_value());

  EXPECT_FALSE(analyzer->is_trivial()); // 5 > default max (4)
  EXPECT_FALSE(analyzer->is_trivial(4)); // 5 > 4
  EXPECT_TRUE(analyzer->is_trivial(5)); // 5 <= 5 (exact boundary)
}

TEST_F(KotlinLambdaAnalyzerTest, NonTrivialLambda_Capturing) {
  auto* capturing_lambda_type = DexType::make_type("LAnalyzerTestCapturing$1;");
  auto* kotlin_function_type =
      DexType::make_type("Lkotlin/jvm/functions/Function0;");

  ClassCreator creator(capturing_lambda_type);
  creator.set_super(type::kotlin_jvm_internal_Lambda());
  creator.add_interface(kotlin_function_type);

  // Add an instance field to represent a captured variable
  auto* field_type = DexType::make_type("Ljava/lang/String;");
  const auto* field_name = DexString::make_string("captured$0");
  auto* field =
      DexField::make_field(capturing_lambda_type, field_name, field_type)
          ->make_concrete(ACC_PRIVATE | ACC_FINAL);
  creator.add_field(field);

  // Add invoke method with only 2 instructions
  auto* invoke_proto = DexProto::make_proto(type::java_lang_Object(),
                                            DexTypeList::make_type_list({}));
  auto* invoke_method =
      DexMethod::make_method(capturing_lambda_type,
                             DexString::make_string("invoke"), invoke_proto)
          ->make_concrete(ACC_PUBLIC, true);
  auto code = std::make_unique<IRCode>(invoke_method, 1);
  code->push_back(new IRInstruction(OPCODE_CONST));
  code->push_back(new IRInstruction(OPCODE_RETURN_OBJECT));
  invoke_method->set_code(std::move(code));
  creator.add_method(invoke_method);

  const auto* capturing_lambda_class = creator.create();

  auto analyzer = KotlinLambdaAnalyzer::analyze(capturing_lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  // Capturing lambdas are never trivial, even with few instructions
  EXPECT_FALSE(analyzer->is_trivial());
}

TEST_F(KotlinLambdaAnalyzerTest, NonTrivialLambda_NoInvokeMethod) {
  const auto* lambda_class = create_lambda_without_invoke();

  auto analyzer = KotlinLambdaAnalyzer::analyze(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_FALSE(analyzer->is_trivial());
}

TEST_F(KotlinLambdaAnalyzerTest, NonLambdaClass) {
  auto* const non_lambda_type = DexType::make_type("LAnalyzerTestNonLambda;");
  ClassCreator creator(non_lambda_type);
  creator.set_super(type::java_lang_Object());
  const auto* non_lambda_class = creator.create();

  auto analyzer = KotlinLambdaAnalyzer::analyze(non_lambda_class);
  EXPECT_FALSE(analyzer.has_value());
}

TEST_F(KotlinLambdaAnalyzerTest, IsNonCapturing) {
  auto* kotlin_function_type =
      DexType::make_type("Lkotlin/jvm/functions/Function0;");

  // Non-capturing lambda (no instance fields)
  {
    auto* lambda_type = DexType::make_type("LAnalyzerTestNonCapturing$1;");
    ClassCreator creator(lambda_type);
    creator.set_super(type::kotlin_jvm_internal_Lambda());
    creator.add_interface(kotlin_function_type);

    const auto* lambda_class = creator.create();
    auto analyzer = KotlinLambdaAnalyzer::analyze(lambda_class);
    ASSERT_TRUE(analyzer.has_value());
    EXPECT_TRUE(analyzer->is_non_capturing());
  }

  // Capturing lambda (has instance field)
  {
    auto* lambda_type = DexType::make_type("LAnalyzerTestCapturing$2;");
    ClassCreator creator(lambda_type);
    creator.set_super(type::kotlin_jvm_internal_Lambda());
    creator.add_interface(kotlin_function_type);

    auto* field_type = DexType::make_type("Ljava/lang/String;");
    const auto* field_name = DexString::make_string("captured$0");
    auto* field = DexField::make_field(lambda_type, field_name, field_type)
                      ->make_concrete(ACC_PRIVATE | ACC_FINAL);
    creator.add_field(field);

    const auto* lambda_class = creator.create();
    auto analyzer = KotlinLambdaAnalyzer::analyze(lambda_class);
    ASSERT_TRUE(analyzer.has_value());
    EXPECT_FALSE(analyzer->is_non_capturing());
  }
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_ProperLambda) {
  const auto* lambda_class = create_lambda_with_invoke();
  auto analyzer = KotlinLambdaAnalyzer::analyze(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  DexMethod* invoke = analyzer->get_invoke_method();
  ASSERT_THAT(invoke, NotNull());
  EXPECT_EQ(invoke->str(), "invoke");
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_WithoutInvoke) {
  const auto* lambda_class = create_lambda_without_invoke();
  auto analyzer = KotlinLambdaAnalyzer::analyze(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_THAT(analyzer->get_invoke_method(), IsNull());
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_MultipleInvokes) {
  const auto* lambda_class = create_lambda_with_multiple_invokes();
  auto analyzer = KotlinLambdaAnalyzer::analyze(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_THAT(analyzer->get_invoke_method(), IsNull());
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_NonPublicInvoke) {
  const auto* lambda_class = create_lambda_with_non_public_invoke();
  auto analyzer = KotlinLambdaAnalyzer::analyze(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_THAT(analyzer->get_invoke_method(), IsNull());
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_SyntheticInvoke) {
  const auto* lambda_class = create_lambda_with_synthetic_invoke();
  auto analyzer = KotlinLambdaAnalyzer::analyze(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_THAT(analyzer->get_invoke_method(), IsNull());
}
