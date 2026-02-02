/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TypeUtil.h"

#include "Creators.h"
#include "IRCode.h"
#include "RedexTest.h"

class TypeUtilTest : public RedexTest {

 protected:
  const std::array<char, 9> PRIMS{
      {'Z', 'B', 'S', 'C', 'I', 'J', 'F', 'D', 'V'}};

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
};

TEST_F(TypeUtilTest, test_reference_type_wrappers) {
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("Z")),
            DexType::make_type("Ljava/lang/Boolean;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("B")),
            DexType::make_type("Ljava/lang/Byte;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("S")),
            DexType::make_type("Ljava/lang/Short;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("C")),
            DexType::make_type("Ljava/lang/Character;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("I")),
            DexType::make_type("Ljava/lang/Integer;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("J")),
            DexType::make_type("Ljava/lang/Long;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("F")),
            DexType::make_type("Ljava/lang/Float;"));
  EXPECT_EQ(type::get_boxed_reference_type(DexType::make_type("D")),
            DexType::make_type("Ljava/lang/Double;"));
}

TEST_F(TypeUtilTest, is_valid_empty) {
  using namespace type;

  EXPECT_FALSE(is_valid(""));
}

TEST_F(TypeUtilTest, is_valid_primitive) {
  using namespace type;

  for (char c : PRIMS) {
    std::string str(1, c);
    EXPECT_TRUE(is_valid(str)) << str;
    str.append("X");
    EXPECT_FALSE(is_valid(str)) << str;
  }
}

TEST_F(TypeUtilTest, is_valid_primitive_array) {
  using namespace type;

  EXPECT_FALSE(is_valid("["));

  std::string prefix = "[";

  for (char c : PRIMS) {
    std::string ok = prefix + c;
    EXPECT_TRUE(is_valid(ok)) << ok;

    std::string not_ok = ok + 'X';
    EXPECT_FALSE(is_valid(not_ok)) << not_ok;

    std::string nested_ok = prefix + ok;
    EXPECT_TRUE(is_valid(nested_ok)) << nested_ok;

    std::string nested_not_ok = nested_ok + 'X';
    EXPECT_FALSE(is_valid(nested_not_ok)) << nested_not_ok;
  }
}

namespace {

std::array<std::pair<std::string, bool>, 8> REF_SAMPLES = {
    std::make_pair<std::string, bool>("Foo", false),
    std::make_pair<std::string, bool>("LFoo", false),
    std::make_pair<std::string, bool>("LFoo;", true),
    std::make_pair<std::string, bool>("LFoo;;", false),

    std::make_pair<std::string, bool>("LFoo_Bar-Baz$A0123;", true),

    std::make_pair<std::string, bool>("Lfoo/bar/Baz;", true),
    std::make_pair<std::string, bool>("Lfoo;bar/Baz;", false),
    std::make_pair<std::string, bool>("Lfoo//Baz;", false),
};

} // namespace

TEST_F(TypeUtilTest, is_valid_reference) {
  using namespace type;

  for (const auto& p : REF_SAMPLES) {
    EXPECT_EQ(p.second, is_valid(p.first)) << p.first;
  }
}

TEST_F(TypeUtilTest, is_valid_reference_array) {
  using namespace type;

  std::string prefix = "[";

  for (const auto& p : REF_SAMPLES) {
    std::string single = prefix + p.first;
    EXPECT_EQ(p.second, is_valid(single)) << single;

    std::string not_ok = single + 'X';
    EXPECT_FALSE(is_valid(not_ok)) << not_ok;

    std::string nested = prefix + single;
    EXPECT_EQ(p.second, is_valid(nested)) << nested;

    std::string nested_not_ok = nested + 'X';
    EXPECT_FALSE(is_valid(nested_not_ok)) << nested_not_ok;
  }
}

TEST_F(TypeUtilTest, is_valid_array) {
  using namespace type;

  // Invalid arrays.
  EXPECT_FALSE(is_valid("["));
  EXPECT_FALSE(is_valid("[["));
  EXPECT_FALSE(is_valid("[o"));
  EXPECT_FALSE(is_valid("[L;"));
  EXPECT_FALSE(is_valid("[;"));
}

TEST_F(TypeUtilTest, check_cast_array) {
  using namespace type;

  EXPECT_FALSE(check_cast(DexType::make_type("[I"), DexType::make_type("[J")));
  EXPECT_FALSE(check_cast(DexType::make_type("[Z"), DexType::make_type("[B")));
  EXPECT_FALSE(check_cast(DexType::make_type("[F"), DexType::make_type("[D")));
  EXPECT_TRUE(check_cast(DexType::make_type("[I"),
                         DexType::make_type("Ljava/lang/Object;")));

  EXPECT_TRUE(check_cast(DexType::make_type("[Ljava/lang/Object;"),
                         DexType::make_type("[Ljava/lang/Object;")));
  EXPECT_TRUE(check_cast(DexType::make_type("[Ljava/lang/Object;"),
                         DexType::make_type("Ljava/lang/Object;")));
  EXPECT_TRUE(check_cast(DexType::make_type("[[Ljava/lang/Object;"),
                         DexType::make_type("[[Ljava/lang/Object;")));
  EXPECT_FALSE(check_cast(DexType::make_type("[Ljava/lang/Object;"),
                          DexType::make_type("[[Ljava/lang/Object;")));
  EXPECT_TRUE(check_cast(DexType::make_type("[[Ljava/lang/Object;"),
                         DexType::make_type("[Ljava/lang/Object;")));
}

TEST_F(TypeUtilTest, same_package) {
  using namespace type;

  EXPECT_TRUE(same_package(DexType::make_type("Ljava/lang/Object;"),
                           DexType::make_type("Ljava/lang/Object;")));

  EXPECT_TRUE(same_package(DexType::make_type("Ljava/lang/Object;"),
                           DexType::make_type("Ljava/lang/String;")));

  EXPECT_FALSE(same_package(DexType::make_type("Ljava/lang/Object;"),
                            DexType::make_type("Ljava/util/List;")));
}

TEST_F(TypeUtilTest, same_package_sub_package) {
  using namespace type;

  EXPECT_TRUE(same_package(DexType::make_type("Ljava/lang/Object;"),
                           DexType::make_type("Ljava/lang/Object;")));

  EXPECT_FALSE(same_package(DexType::make_type("Ljava/lang/Object;"),
                            DexType::make_type("Ljava/lang/reflect/Method;")));
}

class IsKotlinLambdaTest : public TypeUtilTest {
 protected:
  DexType* const kotlin_function_type{
      DexType::make_type("Lkotlin/jvm/functions/Function1;")};
  DexType* const non_kotlin_function_interface_type{
      DexType::make_type("Ljava/lang/Runnable;")};
};

class LambdaBasedFunction1LambdaTest
    : public IsKotlinLambdaTest,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(LambdaBasedFunction1LambdaTest, main) {
  using namespace type;
  const auto* kotlin_lambda_class = create_non_capturing_lambda(GetParam(), 1);
  EXPECT_TRUE(is_kotlin_lambda(kotlin_lambda_class));
}

INSTANTIATE_TEST_SUITE_P(LambdaBasedFunction1LambdaTests,
                         LambdaBasedFunction1LambdaTest,
                         ::testing::Values("LKotlinLambda$0;",
                                           "LKotlinLambda$1;",
                                           "LKotlinLambda$12;",
                                           "LKotlinLambda$123;"));

TEST_F(IsKotlinLambdaTest, LambdaBasedFunctionNLambda) {
  using namespace type;
  // Create a Kotlin lambda class with kotlin.jvm.internal.Lambda as super class
  // and implementing a Kotlin function interface for more than 22 arguments
  auto* lambda_n_type = DexType::make_type("LKotlinLambda$3;");
  auto* kotlin_function_n_type =
      DexType::make_type("Lkotlin/jvm/functions/FunctionN;");

  ClassCreator lambda_n_creator(lambda_n_type);
  lambda_n_creator.set_super(kotlin_jvm_internal_Lambda());
  lambda_n_creator.add_interface(kotlin_function_n_type);
  auto* kotlin_lambda_n_class = lambda_n_creator.create();
  EXPECT_TRUE(is_kotlin_lambda(kotlin_lambda_n_class));
}

class LambdaBasedFunction1NotLambdaTest
    : public IsKotlinLambdaTest,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(LambdaBasedFunction1NotLambdaTest, main) {
  using namespace type;
  // Create a Kotlin lambda class with kotlin.jvm.internal.Lambda as super class
  // and implementing a Kotlin function interface
  auto* lambda_type = DexType::make_type(GetParam());

  ClassCreator lambda_creator(lambda_type);
  lambda_creator.set_super(kotlin_jvm_internal_Lambda());
  lambda_creator.add_interface(kotlin_function_type);
  auto* kotlin_lambda_class = lambda_creator.create();
  EXPECT_FALSE(is_kotlin_lambda(kotlin_lambda_class));
}

INSTANTIATE_TEST_SUITE_P(LambdaBasedFunction1NotLambdaTests,
                         LambdaBasedFunction1NotLambdaTest,
                         ::testing::Values("LNothingAfterDollar$;",
                                           "LNodigitAfterDollar$a;",
                                           "LNamedClass;"));
class ObjectBasedLambdaTest
    : public IsKotlinLambdaTest,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(ObjectBasedLambdaTest, main) {
  using namespace type;
  // Create a class with java.lang.Object as super class and implementing a
  // Kotlin function interface (also valid for Kotlin lambdas)
  auto* obj_lambda_type = DexType::make_type(GetParam());

  ClassCreator obj_lambda_creator(obj_lambda_type);
  obj_lambda_creator.set_super(java_lang_Object());
  obj_lambda_creator.add_interface(kotlin_function_type);
  auto* obj_lambda_class = obj_lambda_creator.create();
  EXPECT_TRUE(is_kotlin_lambda(obj_lambda_class));
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
    : public IsKotlinLambdaTest,
      public ::testing::WithParamInterface<std::string> {};

TEST_P(ObjectBasedNonLambdaTest, main) {
  using namespace type;
  // Create a class with java.lang.Object as super class and implementing a
  // Kotlin function interface (also valid for Kotlin lambdas)
  auto* obj_lambda_type = DexType::make_type(GetParam());

  ClassCreator obj_lambda_creator(obj_lambda_type);
  obj_lambda_creator.set_super(java_lang_Object());
  obj_lambda_creator.add_interface(kotlin_function_type);
  auto* obj_lambda_class = obj_lambda_creator.create();
  EXPECT_FALSE(is_kotlin_lambda(obj_lambda_class));
}

INSTANTIATE_TEST_SUITE_P(
    ObjectBasedNonLambdaTests,
    ObjectBasedNonLambdaTest,
    ::testing::Values("LObjectLambdaWithEmptyEnd$$ExternalSyntheticLambda;",
                      "LObjectLambdaWithEmptyEnd$$Lambda$;",
                      "LObjectLambdaWithLetterEnd$$ExternalSyntheticLambdax;",
                      "LObjectLambdaWithLetterEnd$$Lambda$x;",
                      "LNonD8DesugaredAnonymous$1;",
                      "LNamedClass;"));

TEST_F(IsKotlinLambdaTest, WrongInterface) {
  using namespace type;
  // Create a class with kotlin.jvm.internal.Lambda as super class but
  // implementing a non-Kotlin function interface
  auto* wrong_interface_type = DexType::make_type("LWrongInterface$1;");

  ClassCreator wrong_interface_creator(wrong_interface_type);
  wrong_interface_creator.set_super(kotlin_jvm_internal_Lambda());
  wrong_interface_creator.add_interface(non_kotlin_function_interface_type);
  auto* wrong_interface_class = wrong_interface_creator.create();
  EXPECT_FALSE(is_kotlin_lambda(wrong_interface_class));
}

TEST_F(IsKotlinLambdaTest, MultiInterface) {
  using namespace type;
  // Create a class with kotlin.jvm.internal.Lambda as super class but
  // implementing multiple interfaces
  auto* multi_interface_type = DexType::make_type("LMultiInterface$1;");

  ClassCreator multi_interface_creator(multi_interface_type);
  multi_interface_creator.set_super(kotlin_jvm_internal_Lambda());
  multi_interface_creator.add_interface(kotlin_function_type);
  multi_interface_creator.add_interface(non_kotlin_function_interface_type);
  auto* multi_interface_class = multi_interface_creator.create();
  EXPECT_FALSE(is_kotlin_lambda(multi_interface_class));
}

TEST_F(IsKotlinLambdaTest, WrongSuper) {
  using namespace type;
  // Create a class with wrong super class
  auto* wrong_super_type = DexType::make_type("LWrongSuper$1;");

  ClassCreator wrong_super_creator(wrong_super_type);
  wrong_super_creator.set_super(java_lang_String());
  wrong_super_creator.add_interface(kotlin_function_type);
  auto* wrong_super_class = wrong_super_creator.create();
  EXPECT_FALSE(is_kotlin_lambda(wrong_super_class));
}

TEST_F(IsKotlinLambdaTest, NoInterface) {
  using namespace type;
  // Create a class with no interfaces
  auto* no_interface_type = DexType::make_type("LNoInterface$1;");

  ClassCreator no_interface_creator(no_interface_type);
  no_interface_creator.set_super(kotlin_jvm_internal_Lambda());
  auto* no_interface_class = no_interface_creator.create();
  EXPECT_FALSE(is_kotlin_lambda(no_interface_class));
}

TEST_F(IsKotlinLambdaTest, UnnumberedFunction) {
  using namespace type;
  // Create an otherwise Kotlin lambda class that implements an otherwise Kotlin
  // function interface without a number.
  auto* unnumbered_function_class_type =
      DexType::make_type("LUnnumberedFunction$1;");
  auto* const unnumbered_kotlin_function_type =
      DexType::make_type("Lkotlin/jvm/functions/Function;");
  ClassCreator unnumbered_kotlin_function_creator(
      unnumbered_function_class_type);
  unnumbered_kotlin_function_creator.set_super(kotlin_jvm_internal_Lambda());
  unnumbered_kotlin_function_creator.add_interface(
      unnumbered_kotlin_function_type);
  auto* unnumbered_kotlin_function_class =
      unnumbered_kotlin_function_creator.create();
  EXPECT_FALSE(is_kotlin_lambda(unnumbered_kotlin_function_class));
}
