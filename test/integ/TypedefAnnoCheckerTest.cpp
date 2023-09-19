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

  void gather_typedef_values(
      TypedefAnnoCheckerPass pass,
      DexClass* cls,
      ConcurrentMap<const DexClass*, std::unordered_set<std::string>>&
          strdef_constants,
      ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>>&
          intdef_constants) {
    pass.gather_typedef_values(cls, strdef_constants, intdef_constants);
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
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
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
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
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
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
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
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
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
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
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
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
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
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
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
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
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
  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestConstReturn) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testConstReturn:()I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestInvalidConstReturn) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testInvalidConstReturn:()I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(checker.error() ==
              "TypedefAnnoCheckerPass: the value 5 in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidConstReturn:()I either does "
              "not have the typedef annotation  Linteg/TestIntDef; or was not "
              "declared as one of the possible values in the annotation.");
}

TEST_F(TypedefAnnoCheckerTest, TestInvalidConstReturn2) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testInvalidConstReturn2:()I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(checker.error() ==
              "TypedefAnnoCheckerPass: the value 5 in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidConstReturn2:()I either does "
              "not have the typedef annotation  Linteg/TestIntDef; or was not "
              "declared as one of the possible values in the annotation.");
}

TEST_F(TypedefAnnoCheckerTest, TestInvalidConstStrReturn) {
  auto scope = build_class_scope(stores);
  auto method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/"
          "TypedefAnnoCheckerTest;.testInvalidConstStrReturn:()Ljava/lang/"
          "String;")
          ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(
      checker.error() ==
      "TypedefAnnoCheckerPass: the value five in method "
      "Lcom/facebook/redextest/"
      "TypedefAnnoCheckerTest;.testInvalidConstStrReturn:()Ljava/lang/String; "
      "either does not have the typedef annotation  Linteg/TestStringDef; or "
      "was not declared as one of the possible values in the annotation.");
}

TEST_F(TypedefAnnoCheckerTest, TestInvalidConstInvokeStatic) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testInvalidConstInvokeStatic:()I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(checker.error() ==
              "TypedefAnnoCheckerPass: the value 5 in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidConstInvokeStatic:()I either "
              "does not have the typedef annotation  Linteg/TestIntDef; or was "
              "not declared as one of the possible values in the annotation. "
              "This error occured while invoking the method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testIntAnnoInvokeStatic:(I)I");
}

TEST_F(TypedefAnnoCheckerTest, TestInvalidConstInvokeStatic2) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testInvalidConstInvokeStatic2:()I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(checker.error() ==
              "TypedefAnnoCheckerPass: the value 5 in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidConstInvokeStatic2:()I "
              "either does not have the typedef annotation  Linteg/TestIntDef; "
              "or was not declared as one of the possible values in the "
              "annotation. This error occured while invoking the method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testIntAnnoInvokeStatic:(I)I");
}

TEST_F(TypedefAnnoCheckerTest, TestMultipleBlocksInt) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testMultipleBlocksInt:(I)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestMultipleBlocksString) {
  auto scope = build_class_scope(stores);
  auto method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/"
          "TypedefAnnoCheckerTest;.testMultipleBlocksString:(Ljava/lang/"
          "String;)Ljava/lang/"
          "String;")
          ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestInvalidMultipleBlocksString) {
  auto scope = build_class_scope(stores);
  auto method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/"
          "TypedefAnnoCheckerTest;.testInvalidMultipleBlocksString:(Ljava/lang/"
          "String;)Ljava/lang/"
          "String;")
          ->as_def();
  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(checker.error() ==
              "TypedefAnnoCheckerPass: the method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidMultipleBlocksString:(Ljava/"
              "lang/String;)Ljava/lang/String; changes a value with annotation "
              " Linteg/TestStringDef; midway, which is unsafe.");
}

TEST_F(TypedefAnnoCheckerTest, TestNonConstInt) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testNonConstInt:(I)I")
                    ->as_def();
  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(
      checker.error() ==
      "TypedefAnnoCheckerPass: the annotation  Linteg/TestIntDef; annotates a "
      "non-constant integral in method "
      "Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testNonConstInt:(I)I . "
      "Check that the type and annotation in the method signature are valid "
      "and the value has not changed throughout the method.");
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

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
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

TEST_F(TypedefAnnoCheckerTest, TestJoiningTwoAnnotations) {
  auto scope = build_class_scope(stores);
  auto method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/"
          "TypedefAnnoCheckerTest;.testJoiningTwoAnnotations:(Ljava/lang/"
          "String;Ljava/lang/String;)Ljava/lang/String;")
          ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(
      checker.error() ==
      "TypedefAnnoCheckerPass: the method "
      "Lcom/facebook/redextest/"
      "TypedefAnnoCheckerTest;.testJoiningTwoAnnotations:(Ljava/lang/"
      "String;Ljava/lang/String;)Ljava/lang/String; changes a value "
      "with annotation  Linteg/TestStringDef; midway, which is unsafe.");
}

TEST_F(TypedefAnnoCheckerTest, TestJoiningTwoAnnotations2) {
  auto scope = build_class_scope(stores);
  auto method =
      DexMethod::get_method(
          "Lcom/facebook/redextest/"
          "TypedefAnnoCheckerTest;.testJoiningTwoAnnotations2:(Ljava/lang/"
          "String;Ljava/lang/String;)Ljava/lang/String;")
          ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}

TEST_F(TypedefAnnoCheckerTest, TestReassigningInt) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testReassigningInt:(II)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<std::string>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_TRUE(checker.complete());
}
