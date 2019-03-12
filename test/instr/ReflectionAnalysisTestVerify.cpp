/**
 * Copyright (c) Facebook, Inc. and its affiliates.
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
  for (const auto it : reflection_sites) {
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
  EXPECT_EQ(to_string(analysis.get_reflection_sites()), expect_output);
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

TEST_F(PostVerify, TestAbstractDomain) {
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

TEST_F(PostVerify, JoinSameClassType) {
  test_analysis(
      classes, "getClassJoinSame",
      "MOVE_RESULT_OBJECT v1 {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
GOTO  {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
IOPCODE_MOVE_RESULT_PSEUDO_OBJECT v1 {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
INVOKE_VIRTUAL v1, Ljava/lang/Class;.getName:()Ljava/lang/String; {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
MOVE_RESULT_OBJECT v1 {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n");
}

TEST_F(PostVerify, JoinDifferentClassType) {
  test_analysis(
      classes, "getClassJoinDifferent",
      "MOVE_RESULT_OBJECT v1 {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
GOTO  {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
MOVE_RESULT_OBJECT v1 {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Bar;}(REFLECTION)}\n\
INVOKE_VIRTUAL v1, Ljava/lang/Class;.getName:()Ljava/lang/String; {1, CLASS{}(REFLECTION);4294967294, CLASS{}(REFLECTION)}\n\
MOVE_RESULT_OBJECT v1 {1, CLASS{}(REFLECTION)}\n");
}

TEST_F(PostVerify, JoinClassTypeWithEmpty) {
  test_analysis(
      classes, "getClassJoinEmpty",
      "MOVE_RESULT_OBJECT v1 {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
INVOKE_VIRTUAL v1, Ljava/lang/Class;.getPackage:()Ljava/lang/Package; {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
INVOKE_VIRTUAL v1, Ljava/lang/Class;.getName:()Ljava/lang/String; {1, CLASS{}}\n\
MOVE_RESULT_OBJECT v1 {1, CLASS{}}\n");
}

TEST_F(PostVerify, JoinSameString) {
  test_analysis(
      classes, "getStringJoinSame",
      "MOVE_RESULT_OBJECT v1 {4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n\
RETURN_OBJECT v1 {1, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION);4294967294, CLASS{Lcom/facebook/redextest/ReflectionAnalysisTest$Foo;}(REFLECTION)}\n");
}

TEST_F(PostVerify, JoinDifferentString) {
  test_analysis(classes, "getStringJoinDifferent",
                "MOVE_RESULT_OBJECT v1 {4294967294, CLASS{}(REFLECTION)}\n\
RETURN_OBJECT v1 {1, CLASS{}(REFLECTION);4294967294, CLASS{}(REFLECTION)}\n");
}

TEST_F(PostVerify, JoinStringWithEmpty) {
  test_analysis(classes, "getStringJoinEmpty",
                "MOVE_RESULT_OBJECT v1 {4294967294, CLASS{}(REFLECTION)}\n\
RETURN_OBJECT v1 {1, CLASS{}(REFLECTION);4294967294, CLASS{}(REFLECTION)}\n");
}
