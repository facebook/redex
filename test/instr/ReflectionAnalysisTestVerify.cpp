/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ReflectionAnalysis.h"
#include "Show.h"
#include "verify/VerifyUtil.h"

using namespace reflection;

std::string to_string(const ReflectionSites& reflection_sites) {
  std::ostringstream out;
  for (const auto& it : reflection_sites) {
    out << SHOW(it.first) << " {";
    for (auto iit = it.second.begin(); iit != it.second.end(); ++iit) {
      out << iit->first << ", " << iit->second;
      if (std::next(iit) != it.second.end()) {
        out << ";";
      }
    }
    out << "}" << std::endl;
  }

  return out.str();
}

void test_analysis(const DexClasses& classes,
                   const char* method_name,
                   const char* expect_output) {
  auto cls = find_class_named(
      classes, "Lcom/facebook/redextest/ReflectionAnalysisTest$Reflector;");
  ASSERT_NE(cls, nullptr);

  const auto meth = find_vmethod_named(*cls, method_name);
  ASSERT_NE(meth, nullptr);
  meth->balloon();
  ReflectionAnalysis analysis(meth);
  EXPECT_TRUE(analysis.has_found_reflection());
  std::string actual_output = to_string(analysis.get_reflection_sites());
  EXPECT_EQ(actual_output, expect_output);
}

using Methods = std::set<std::pair<std::string, std::vector<DexType*>>>;

void get_reflected_methods_by_test(Methods& out,
                                   const DexClasses& classes,
                                   const char* method_name) {

  auto cls = find_class_named(
      classes, "Lcom/facebook/redextest/ReflectionAnalysisTest$Reflector;");
  ASSERT_NE(cls, nullptr);

  const auto meth = find_vmethod_named(*cls, method_name);
  ASSERT_NE(meth, nullptr);
  meth->balloon();
  ReflectionAnalysis analysis(meth);
  ASSERT_TRUE(analysis.has_found_reflection());

  std::vector<DexType*> empty;
  DexType* test_cls =
      DexType::make_type("Lcom/facebook/redextest/ReflectionAnalysisTest$Baz;");
  for (const auto& site : analysis.get_reflection_sites()) {
    for (const auto& it : site.second) {
      const auto& aobj = it.second.first; // abstract object
      if (aobj.obj_kind == reflection::AbstractObjectKind::METHOD &&
          aobj.dex_type == test_cls) {
        out.emplace(std::make_pair(aobj.dex_string->str_copy(),
                                   aobj.dex_type_array ? *(aobj.dex_type_array)
                                                       : empty));
      }
    }
  }
}

template <typename F>
void test_operation(const AbstractObject& operand,
                    F&& operation,
                    const AbstractObject& expectValue) {
  // Need to make a copy since the operation mutate the operand in-place
  auto resultValue = operand;
  auto resultKind = operation(resultValue);
  EXPECT_EQ(resultKind, sparta::AbstractValueKind::Value);
  EXPECT_EQ(resultValue, expectValue);
}

void test_join_meet(const AbstractObject& foo,
                    const AbstractObject& bar,
                    const AbstractObject& generic) {
  EXPECT_TRUE(generic.leq(generic));
  EXPECT_FALSE(generic.leq(foo));
  EXPECT_FALSE(generic.leq(bar));
  EXPECT_TRUE(foo.leq(generic));
  EXPECT_TRUE(foo.leq(foo));
  EXPECT_FALSE(foo.leq(bar));
  EXPECT_TRUE(bar.leq(generic));
  EXPECT_FALSE(bar.leq(foo));
  EXPECT_TRUE(bar.leq(bar));

  auto join_with_foo = [&foo](auto&& obj) { return obj.join_with(foo); };
  auto join_with_bar = [&bar](auto&& obj) { return obj.join_with(bar); };
  auto join_with_generic = [&generic](auto&& obj) {
    return obj.join_with(generic);
  };
  test_operation(generic, join_with_generic, generic);
  test_operation(generic, join_with_foo, generic);
  test_operation(generic, join_with_bar, generic);
  test_operation(foo, join_with_generic, generic);
  test_operation(foo, join_with_foo, foo);
  test_operation(foo, join_with_bar, generic);
  test_operation(bar, join_with_generic, generic);
  test_operation(bar, join_with_foo, generic);
  test_operation(bar, join_with_bar, bar);

  auto meet_with_foo = [&foo](auto&& obj) { return obj.meet_with(foo); };
  auto meet_with_bar = [&bar](auto&& obj) { return obj.meet_with(bar); };
  auto meet_with_generic = [&generic](auto&& obj) {
    return obj.meet_with(generic);
  };
  test_operation(generic, meet_with_generic, generic);
  test_operation(generic, meet_with_foo, foo);
  test_operation(generic, meet_with_bar, bar);
  test_operation(foo, meet_with_generic, foo);
  test_operation(foo, meet_with_foo, foo);
  test_operation(bar, meet_with_generic, bar);
  test_operation(bar, meet_with_bar, bar);
  auto foo_copy = foo;
  EXPECT_EQ(foo_copy.meet_with(bar), sparta::AbstractValueKind::Bottom);
  auto bar_copy = bar;
  EXPECT_EQ(bar_copy.meet_with(foo), sparta::AbstractValueKind::Bottom);
}

TEST_F(PreVerify, TestAbstractDomain) {
  auto foo_name = DexString::get_string(
      "Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;");
  ASSERT_NE(foo_name, nullptr);
  auto bar_name = DexString::get_string(
      "Lcom/facebook/redextest/ReflectionAnalysisTest$Bar;");
  ASSERT_NE(bar_name, nullptr);
  auto foo_type = DexType::get_type(foo_name);
  ASSERT_NE(foo_type, nullptr);
  auto bar_type = DexType::get_type(bar_name);
  ASSERT_NE(bar_type, nullptr);

  test_join_meet(AbstractObject(AbstractObjectKind::CLASS, foo_type),
                 AbstractObject(AbstractObjectKind::CLASS, bar_type),
                 AbstractObject(AbstractObjectKind::CLASS, nullptr));
  test_join_meet(AbstractObject(AbstractObjectKind::OBJECT, foo_type),
                 AbstractObject(AbstractObjectKind::OBJECT, bar_type),
                 AbstractObject(AbstractObjectKind::OBJECT, nullptr));
  test_join_meet(AbstractObject(foo_name),
                 AbstractObject(bar_name),
                 AbstractObject(nullptr));
  test_join_meet(AbstractObject(AbstractObjectKind::FIELD, foo_type, foo_name),
                 AbstractObject(AbstractObjectKind::FIELD, bar_type, bar_name),
                 AbstractObject(AbstractObjectKind::FIELD, nullptr, nullptr));
  test_join_meet(AbstractObject(AbstractObjectKind::METHOD, foo_type, foo_name),
                 AbstractObject(AbstractObjectKind::METHOD, bar_type, bar_name),
                 AbstractObject(AbstractObjectKind::METHOD, nullptr, nullptr));
}

TEST_F(PreVerify, JoinSameClassType) {
  // clang-format off
  test_analysis(
      classes, "getClassJoinSame",
      "INVOKE_STATIC v1, Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class; {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "MOVE_RESULT_OBJECT v1 {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "GOTO  {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "CONST_CLASS Lcom/facebook/redextest/ReflectionAnalysisTest$Foo; {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v1 {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "INVOKE_VIRTUAL v1, Ljava/lang/Class;.getName:()Ljava/lang/String; {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n");
  // clang-format on
}

TEST_F(PreVerify, JoinDifferentClassType) {
  // clang-format off
  test_analysis(
      classes, "getClassJoinDifferent",
      "INVOKE_STATIC v1, Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class; {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "MOVE_RESULT_OBJECT v1 {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "GOTO  {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "INVOKE_STATIC v1, Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class; {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Bar;}(REFLECTION)}\n"
      "MOVE_RESULT_OBJECT v1 {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Bar;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Bar;}(REFLECTION)}\n"
      "INVOKE_VIRTUAL v1, Ljava/lang/Class;.getName:()Ljava/lang/String; {1, CLASS{}(REFLECTION)}\n");
  // clang-format on
}

TEST_F(PreVerify, JoinClassTypeWithEmpty) {
  // clang-format off
  test_analysis(
      classes, "getClassJoinEmpty",
      "INVOKE_STATIC v1, Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class; {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "MOVE_RESULT_OBJECT v1 {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "INVOKE_VIRTUAL v1, Ljava/lang/Class;.getPackage:()Ljava/lang/Package; {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "INVOKE_VIRTUAL v1, Ljava/lang/Class;.getName:()Ljava/lang/String; {1, CLASS{}}\n");
  // clang-format on
}

TEST_F(PreVerify, JoinSameString) {
  // clang-format off
  test_analysis(
      classes, "getStringJoinSame",
      "INVOKE_STATIC v1, Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class; {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "MOVE_RESULT_OBJECT v1 {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n"
      "RETURN_OBJECT v1 {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n");
  // clang-format on
}

TEST_F(PreVerify, JoinDifferentString) {
  // clang-format off
  test_analysis(classes, "getStringJoinDifferent",
                "INVOKE_STATIC v1, Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class; {4294967294, CLASS{}(REFLECTION)}\n"
                "MOVE_RESULT_OBJECT v1 {1, CLASS{}(REFLECTION);4294967294, CLASS{}(REFLECTION)}\n"
                "RETURN_OBJECT v1 {1, CLASS{}(REFLECTION);4294967294, CLASS{}(REFLECTION)}\n");
  // clang-format on
}

TEST_F(PreVerify, JoinStringWithEmpty) {
  // clang-format off
  test_analysis(classes, "getStringJoinEmpty",
                "INVOKE_STATIC v1, Ljava/lang/Class;.forName:(Ljava/lang/String;)Ljava/lang/Class; {4294967294, CLASS{}(REFLECTION)}\n"
                "MOVE_RESULT_OBJECT v1 {1, CLASS{}(REFLECTION);4294967294, CLASS{}(REFLECTION)}\n"
                "RETURN_OBJECT v1 {1, CLASS{}(REFLECTION);4294967294, CLASS{}(REFLECTION)}\n");
  // clang-format on
}

TEST_F(PreVerify, MethodWithParam) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes, "getMethodWithParam");
  DexType* integer = DexType::make_type("Ljava/lang/Integer;");
  DexType* doublee = DexType::make_type("D");
  EXPECT_TRUE(methods.count({"test", {}}));
  EXPECT_TRUE(methods.count({"test", {integer, doublee}}));
  EXPECT_TRUE(methods.count({"test2", {integer, doublee}}));
}

TEST_F(PreVerify, MethodWithParamOriginal) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes, "getMethodWithParamOriginal");
  DexType* integer = DexType::make_type("Ljava/lang/Integer;");
  DexType* doublee = DexType::make_type("D");
  EXPECT_TRUE(methods.count({"test", {integer, doublee}}));
  EXPECT_FALSE(methods.count({"test", {}}));
}

TEST_F(PreVerify, MethodWithParamInvalidatedArgs1) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes,
                                "getMethodWithParamInvalidatedArgs1");
  EXPECT_EQ(methods.size(), 1);
  EXPECT_TRUE(methods.count({"test", {}}));
}

TEST_F(PreVerify, MethodWithParamInvalidatedArgs2) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes,
                                "getMethodWithParamInvalidatedArgs2");
  EXPECT_EQ(methods.size(), 1);
  EXPECT_TRUE(methods.count({"test", {}}));
}

TEST_F(PreVerify, MethodWithParamInvalidatedArgs3) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes,
                                "getMethodWithParamInvalidatedArgs3");
  EXPECT_EQ(methods.size(), 1);
  EXPECT_TRUE(methods.count({"test", {}}));
}

TEST_F(PreVerify, MethodWithParamInvalidatedArgs4) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes,
                                "getMethodWithParamInvalidatedArgs4");
  EXPECT_EQ(methods.size(), 1);
}

TEST_F(PreVerify, ConstructorWithParam) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes, "getConstructorWithParam");
  DexType* integer = DexType::make_type("Ljava/lang/Integer;");
  DexType* doublee = DexType::make_type("D");
  EXPECT_TRUE(methods.count({"<init>", {}}));
  EXPECT_TRUE(methods.count({"<init>", {integer, doublee}}));
  EXPECT_TRUE(methods.count({"<init>", {integer, doublee}}));
}

TEST_F(PreVerify, ConstructorWithParamInvalidatedArgs1) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes,
                                "getConstructorWithParamInvalidatedArgs1");
  EXPECT_EQ(methods.size(), 1);
  EXPECT_TRUE(methods.count({"<init>", {}}));
}

TEST_F(PreVerify, ConstructorWithParamInvalidatedArgs2) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes,
                                "getConstructorWithParamInvalidatedArgs2");
  EXPECT_EQ(methods.size(), 1);
  EXPECT_TRUE(methods.count({"<init>", {}}));
}

TEST_F(PreVerify, ConstructorWithParamInvalidatedArgs3) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes,
                                "getConstructorWithParamInvalidatedArgs3");
  EXPECT_EQ(methods.size(), 1);
  EXPECT_TRUE(methods.count({"<init>", {}}));
}

TEST_F(PreVerify, ConstructorWithParamInvalidatedArgs4) {
  Methods methods;
  get_reflected_methods_by_test(methods, classes,
                                "getConstructorWithParamInvalidatedArgs4");
  EXPECT_EQ(methods.size(), 1);
}
