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
  // Helper to create a well-formed Lambda-based non-capturing Kotlin lambda
  // class with a proper invoke method. This creates lambdas that extend
  // kotlin.jvm.internal.Lambda (as opposed to Object-based lambdas that extend
  // java.lang.Object).
  // @param name The type name for the lambda class.
  // @param arity The number of parameters for the invoke method (default 0).
  static DexClass* create_non_capturing_lambda(std::string_view name,
                                               size_t arity = 0) {
    auto* lambda_type = DexType::make_type(name);
    const auto function_type_name =
        "Lkotlin/jvm/functions/Function" + std::to_string(arity) + ";";
    auto* kotlin_function_type = DexType::make_type(function_type_name);

    ClassCreator creator(lambda_type);
    creator.set_super(type::kotlin_jvm_internal_Lambda());
    creator.add_interface(kotlin_function_type);

    // Build parameter type list based on arity
    DexTypeList::ContainerType param_types;
    for (size_t i = 0; i < arity; ++i) {
      param_types.push_back(type::java_lang_Object());
    }

    // Add a proper public invoke method with code
    auto* invoke_proto = DexProto::make_proto(
        type::java_lang_Object(),
        DexTypeList::make_type_list(std::move(param_types)));
    auto* invoke_method =
        DexMethod::make_method(lambda_type, DexString::make_string("invoke"),
                               invoke_proto)
            ->make_concrete(ACC_PUBLIC, true);
    // Register count: 1 for 'this' + arity for parameters
    auto code = std::make_unique<IRCode>(invoke_method, 1 + arity);
    code->push_back(new IRInstruction(OPCODE_CONST));
    code->push_back(new IRInstruction(OPCODE_RETURN_OBJECT));
    invoke_method->set_code(std::move(code));
    creator.add_method(invoke_method);

    return creator.create();
  }

  static DexType* kotlin_function_type() {
    return DexType::make_type("Lkotlin/jvm/functions/Function1;");
  }

  static DexType* non_kotlin_function_interface_type() {
    return DexType::make_type("Ljava/lang/Runnable;");
  }

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
  auto analyzer = KotlinLambdaAnalyzer::for_class(lambda_class);
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

  auto analyzer = KotlinLambdaAnalyzer::for_class(capturing_lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  // Capturing lambdas are never trivial, even with few instructions
  EXPECT_FALSE(analyzer->is_trivial());
}

TEST_F(KotlinLambdaAnalyzerTest, NonTrivialLambda_NoInvokeMethod) {
  const auto* lambda_class = create_lambda_without_invoke();

  auto analyzer = KotlinLambdaAnalyzer::for_class(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_FALSE(analyzer->is_trivial());
}

TEST_F(KotlinLambdaAnalyzerTest, NonLambdaClass) {
  auto* const non_lambda_type = DexType::make_type("LAnalyzerTestNonLambda;");
  ClassCreator creator(non_lambda_type);
  creator.set_super(type::java_lang_Object());
  const auto* non_lambda_class = creator.create();

  auto analyzer = KotlinLambdaAnalyzer::for_class(non_lambda_class);
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
    auto analyzer = KotlinLambdaAnalyzer::for_class(lambda_class);
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
    auto analyzer = KotlinLambdaAnalyzer::for_class(lambda_class);
    ASSERT_TRUE(analyzer.has_value());
    EXPECT_FALSE(analyzer->is_non_capturing());
  }
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_ProperLambda) {
  const auto* lambda_class = create_lambda_with_invoke();
  auto analyzer = KotlinLambdaAnalyzer::for_class(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  DexMethod* invoke = analyzer->get_invoke_method();
  ASSERT_THAT(invoke, NotNull());
  EXPECT_EQ(invoke->str(), "invoke");
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_WithoutInvoke) {
  const auto* lambda_class = create_lambda_without_invoke();
  auto analyzer = KotlinLambdaAnalyzer::for_class(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_THAT(analyzer->get_invoke_method(), IsNull());
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_MultipleInvokes) {
  const auto* lambda_class = create_lambda_with_multiple_invokes();
  auto analyzer = KotlinLambdaAnalyzer::for_class(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_THAT(analyzer->get_invoke_method(), IsNull());
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_NonPublicInvoke) {
  const auto* lambda_class = create_lambda_with_non_public_invoke();
  auto analyzer = KotlinLambdaAnalyzer::for_class(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_THAT(analyzer->get_invoke_method(), IsNull());
}

TEST_F(KotlinLambdaAnalyzerTest, GetInvokeMethod_SyntheticInvoke) {
  const auto* lambda_class = create_lambda_with_synthetic_invoke();
  auto analyzer = KotlinLambdaAnalyzer::for_class(lambda_class);
  ASSERT_TRUE(analyzer.has_value());
  EXPECT_THAT(analyzer->get_invoke_method(), IsNull());
}

// Tests for lambda detection (KotlinLambdaAnalyzer::for_class)

class LambdaBasedFunction1LambdaTest
    : public KotlinLambdaAnalyzerTest,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(LambdaBasedFunction1LambdaTest, main) {
  const auto* kotlin_lambda_class = create_non_capturing_lambda(GetParam(), 1);
  EXPECT_TRUE(KotlinLambdaAnalyzer::for_class(kotlin_lambda_class));
}

INSTANTIATE_TEST_SUITE_P(LambdaBasedFunction1LambdaTests,
                         LambdaBasedFunction1LambdaTest,
                         ::testing::Values("LKotlinLambda$0;",
                                           "LKotlinLambda$1;",
                                           "LKotlinLambda$12;",
                                           "LKotlinLambda$123;"));

TEST_F(KotlinLambdaAnalyzerTest, LambdaBasedFunctionNLambda) {
  // Create a Kotlin lambda class with kotlin.jvm.internal.Lambda as super class
  // and implementing a Kotlin function interface for more than 22 arguments
  auto* lambda_n_type = DexType::make_type("LKotlinLambda$3;");
  auto* kotlin_function_n_type =
      DexType::make_type("Lkotlin/jvm/functions/FunctionN;");

  ClassCreator lambda_n_creator(lambda_n_type);
  lambda_n_creator.set_super(type::kotlin_jvm_internal_Lambda());
  lambda_n_creator.add_interface(kotlin_function_n_type);
  auto* kotlin_lambda_n_class = lambda_n_creator.create();
  EXPECT_TRUE(KotlinLambdaAnalyzer::for_class(kotlin_lambda_n_class));
}

class LambdaBasedFunction1NotLambdaTest
    : public KotlinLambdaAnalyzerTest,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(LambdaBasedFunction1NotLambdaTest, main) {
  // Create a Kotlin lambda class with kotlin.jvm.internal.Lambda as super class
  // and implementing a Kotlin function interface
  auto* lambda_type = DexType::make_type(GetParam());

  ClassCreator lambda_creator(lambda_type);
  lambda_creator.set_super(type::kotlin_jvm_internal_Lambda());
  lambda_creator.add_interface(kotlin_function_type());
  auto* kotlin_lambda_class = lambda_creator.create();
  EXPECT_FALSE(KotlinLambdaAnalyzer::for_class(kotlin_lambda_class));
}

INSTANTIATE_TEST_SUITE_P(LambdaBasedFunction1NotLambdaTests,
                         LambdaBasedFunction1NotLambdaTest,
                         ::testing::Values("LNothingAfterDollar$;",
                                           "LNodigitAfterDollar$a;",
                                           "LNamedClass;"));

class ObjectBasedLambdaTest
    : public KotlinLambdaAnalyzerTest,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(ObjectBasedLambdaTest, main) {
  // Create a class with java.lang.Object as super class and implementing a
  // Kotlin function interface (also valid for Kotlin lambdas)
  auto* obj_lambda_type = DexType::make_type(GetParam());

  ClassCreator obj_lambda_creator(obj_lambda_type);
  obj_lambda_creator.set_super(type::java_lang_Object());
  obj_lambda_creator.add_interface(kotlin_function_type());
  auto* obj_lambda_class = obj_lambda_creator.create();
  EXPECT_TRUE(KotlinLambdaAnalyzer::for_class(obj_lambda_class));
}

INSTANTIATE_TEST_SUITE_P(
    ObjectBasedLambdaTests,
    ObjectBasedLambdaTest,
    ::testing::Values("LObjectLambda$$ExternalSyntheticLambda0;",
                      "LObjectLambda$$ExternalSyntheticLambda1;",
                      "LObjectLambda$$ExternalSyntheticLambda10;",
                      "LObjectLambda$$ExternalSyntheticLambda112;",
                      "LObjectLambda$$Lambda$0;",
                      "LObjectLambda$$Lambda$1;",
                      "LObjectLambda$$Lambda$10;",
                      "LObjectLambda$$Lambda$112;"));

class ObjectBasedNonLambdaTest
    : public KotlinLambdaAnalyzerTest,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(ObjectBasedNonLambdaTest, main) {
  // Create a class with java.lang.Object as super class and implementing a
  // Kotlin function interface (also valid for Kotlin lambdas)
  auto* obj_lambda_type = DexType::make_type(GetParam());

  ClassCreator obj_lambda_creator(obj_lambda_type);
  obj_lambda_creator.set_super(type::java_lang_Object());
  obj_lambda_creator.add_interface(kotlin_function_type());
  auto* obj_lambda_class = obj_lambda_creator.create();
  EXPECT_FALSE(KotlinLambdaAnalyzer::for_class(obj_lambda_class));
}

INSTANTIATE_TEST_SUITE_P(
    ObjectBasedNonLambdaTests,
    ObjectBasedNonLambdaTest,
    ::testing::Values("LObjectLambdaWithEmptyEnd$$ExternalSyntheticLambda;",
                      "LObjectLambdaWithEmptyEnd$$Lambda$;",
                      "LObjectLambdaWithLetterEnd$$ExternalSyntheticLambdax;",
                      "LObjectLambdaWithLetterEnd$$Lambda$x;",
                      "LNonD8DesugaredAnonymous$1;",
                      "LNamedClass2;"));

TEST_F(KotlinLambdaAnalyzerTest, WrongInterface) {
  // Create a class with kotlin.jvm.internal.Lambda as super class but
  // implementing a non-Kotlin function interface
  auto* wrong_interface_type = DexType::make_type("LWrongInterface$1;");

  ClassCreator wrong_interface_creator(wrong_interface_type);
  wrong_interface_creator.set_super(type::kotlin_jvm_internal_Lambda());
  wrong_interface_creator.add_interface(non_kotlin_function_interface_type());
  auto* wrong_interface_class = wrong_interface_creator.create();
  EXPECT_FALSE(KotlinLambdaAnalyzer::for_class(wrong_interface_class));
}

TEST_F(KotlinLambdaAnalyzerTest, MultiInterface) {
  // Create a class with kotlin.jvm.internal.Lambda as super class but
  // implementing multiple interfaces
  auto* multi_interface_type = DexType::make_type("LMultiInterface$1;");

  ClassCreator multi_interface_creator(multi_interface_type);
  multi_interface_creator.set_super(type::kotlin_jvm_internal_Lambda());
  multi_interface_creator.add_interface(kotlin_function_type());
  multi_interface_creator.add_interface(non_kotlin_function_interface_type());
  auto* multi_interface_class = multi_interface_creator.create();
  EXPECT_FALSE(KotlinLambdaAnalyzer::for_class(multi_interface_class));
}

TEST_F(KotlinLambdaAnalyzerTest, WrongSuper) {
  // Create a class with wrong super class
  auto* wrong_super_type = DexType::make_type("LWrongSuper$1;");

  ClassCreator wrong_super_creator(wrong_super_type);
  wrong_super_creator.set_super(type::java_lang_String());
  wrong_super_creator.add_interface(kotlin_function_type());
  auto* wrong_super_class = wrong_super_creator.create();
  EXPECT_FALSE(KotlinLambdaAnalyzer::for_class(wrong_super_class));
}

TEST_F(KotlinLambdaAnalyzerTest, NoInterface) {
  // Create a class with no interfaces
  auto* no_interface_type = DexType::make_type("LNoInterface$1;");

  ClassCreator no_interface_creator(no_interface_type);
  no_interface_creator.set_super(type::kotlin_jvm_internal_Lambda());
  auto* no_interface_class = no_interface_creator.create();
  EXPECT_FALSE(KotlinLambdaAnalyzer::for_class(no_interface_class));
}

TEST_F(KotlinLambdaAnalyzerTest, UnnumberedFunction) {
  // Create an otherwise Kotlin lambda class that implements an otherwise Kotlin
  // function interface without a number.
  auto* unnumbered_function_class_type =
      DexType::make_type("LUnnumberedFunction$1;");
  auto* const unnumbered_kotlin_function_type =
      DexType::make_type("Lkotlin/jvm/functions/Function;");
  ClassCreator unnumbered_kotlin_function_creator(
      unnumbered_function_class_type);
  unnumbered_kotlin_function_creator.set_super(
      type::kotlin_jvm_internal_Lambda());
  unnumbered_kotlin_function_creator.add_interface(
      unnumbered_kotlin_function_type);
  auto* unnumbered_kotlin_function_class =
      unnumbered_kotlin_function_creator.create();
  EXPECT_FALSE(
      KotlinLambdaAnalyzer::for_class(unnumbered_kotlin_function_class));
}
