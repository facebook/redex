/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <string>

#include "RedexTest.h"
#include "Show.h"
#include "TypedefAnnoCheckerPass.h"

struct TypedefAnnoCheckerTest : public RedexIntegrationTest {
  TypedefAnnoCheckerPass::Config get_config() {
    auto m_config = TypedefAnnoCheckerPass::Config();
    m_config.str_typedef =
        DexType::make_type("Lcom/facebook/redex/annotations/SafeStringDef;");
    m_config.int_typedef =
        DexType::make_type("Lcom/facebook/redex/annotations/SafeIntDef;");
    return m_config;
  }
};

TEST_F(TypedefAnnoCheckerTest, TestValidIntAnnoReturn) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testValidIntAnnoReturn:(I)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestValidStrAnnoReturn) {
  auto scope = build_class_scope(stores);
  auto method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/"
          "TypedefAnnoCheckerTest;.testValidStrAnnoReturn:(Ljava/lang/"
          "String;)Ljava/lang/"
          "String;")
          ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestIntAnnoInvokeStatic) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testIntAnnoInvokeStatic:(I)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestStringAnnoInvokeStatic) {
  auto scope = build_class_scope(stores);
  auto method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/"
          "TypedefAnnoCheckerTest;.testStringAnnoInvokeStatic:(Ljava/lang/"
          "String;)Ljava/lang/"
          "String;")
          ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestWrongAnnotationReturned) {
  auto scope = build_class_scope(stores);
  auto method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/"
          "TypedefAnnoCheckerTest;.testWrongAnnotationReturned:(Ljava/lang/"
          "String;)Ljava/lang/"
          "String;")
          ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(
      checker.error() ==
      "TypedefAnnoCheckerPass: The method "
      "Lcom/facebook/redextest/"
      "TypedefAnnoCheckerTest;.testWrongAnnotationReturned:(Ljava/"
      "lang/String;)Ljava/lang/String; has an annotation Linteg/TestIntDef; in "
      "its method signature, but the returned value contains the annotation  "
      "Linteg/TestStringDef; instead");
}

TEST_F(TypedefAnnoCheckerTest, TestWrongAnnoInvokeStatic) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testWrongAnnoInvokeStatic:(I)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(
      checker.error() ==
      "TypedefAnnoCheckerPass: while invoking "
      "Lcom/facebook/redextest/"
      "TypedefAnnoCheckerTest;.testValidIntAnnoReturn:(I)I in "
      "method "
      "Lcom/facebook/redextest/"
      "TypedefAnnoCheckerTest;.testWrongAnnoInvokeStatic:(I)I, parameter 0 has "
      "the annotation  Linteg/TestStringDef; , but the method expects the "
      "annotation to be Linteg/TestIntDef;");
}

TEST_F(TypedefAnnoCheckerTest, TestIntField) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testIntField:(I)V")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestWrongIntField) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testWrongIntField:(I)V")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(
      checker.error() ==
      "TypedefAnnoCheckerPass: The method "
      "Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testWrongIntField:(I)V "
      "assigned a field wrong_anno_field with annotation  "
      "Linteg/TestStringDef; to a value with annotation  Linteg/TestIntDef;");
}

TEST_F(TypedefAnnoCheckerTest, TestStringField) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testStringField:(Ljava/lang/"
                    "String;)V")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}
TEST_F(TypedefAnnoCheckerTest, TestInvalidType) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testInvalidType:(Lcom/facebook/"
                    "redextest/I;)Lcom/facebook/redextest/I;")
                    ->as_def();
  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<DexClass*, std::unordered_set<std::string>> strdef_constants;
  ConcurrentMap<DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(
      checker.error() ==
      "TypedefAnnoCheckerPass: the annotation  Linteg/TestIntDef; annotates a "
      "value with an incompatible type or a non-constant value in method "
      "Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testInvalidType:(Lcom/"
      "facebook/redextest/I;)Lcom/facebook/redextest/I;");
}
