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
      ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>&
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
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
      "TypedefAnnoCheckerTest;.testWrongAnnotationReturned:(Ljava/lang/"
      "String;)Ljava/lang/String; has an annotation Linteg/TestIntDef; in its "
      "method signature, but the returned value contains the annotation  "
      "Linteg/TestStringDef; instead. failed instruction: RETURN_OBJECT v0.");
}

TEST_F(TypedefAnnoCheckerTest, TestWrongAnnoInvokeStatic) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testWrongAnnoInvokeStatic:(I)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
      strdef_constants;
  ConcurrentMap<const DexClass*, std::unordered_set<uint64_t>> intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_TRUE(checker.error() ==
              "TypedefAnnoCheckerPass: while invoking "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testValidIntAnnoReturn:(I)I in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testWrongAnnoInvokeStatic:(I)I, "
              "parameter 0 has the annotation  Linteg/TestStringDef; , but the "
              "method expects the annotation to be Linteg/TestIntDef; failed "
              "instruction: INVOKE_STATIC v1, "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testValidIntAnnoReturn:(I)I.");
}

TEST_F(TypedefAnnoCheckerTest, TestIntField) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testIntField:(I)V")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
      "Linteg/TestStringDef; to a value with annotation  Linteg/TestIntDef; "
      "failed instruction: IPUT v1, v0, "
      "Lcom/facebook/redextest/TypedefAnnoCheckerTest;.wrong_anno_field:I");
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
  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
              "TypedefAnnoCheckerPass: in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidConstReturn:()I , the int "
              "value 5 does not have the typedef annotation  "
              "Linteg/TestIntDef; attached to it. Check that the value is "
              "annotated and exists in its typedef annotation class.");
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
              "TypedefAnnoCheckerPass: in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidConstReturn2:()I , the int "
              "value 5 does not have the typedef annotation  "
              "Linteg/TestIntDef; attached to it. Check that the value is "
              "annotated and exists in its typedef annotation class.");
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
      "TypedefAnnoCheckerPass: in method "
      "Lcom/facebook/redextest/"
      "TypedefAnnoCheckerTest;.testInvalidConstStrReturn:()Ljava/lang/String; "
      ", the string value five does not have the typedef annotation  "
      "Linteg/TestStringDef; attached to it. Check that the value is annotated "
      "and exists in the typedef annotation class");
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
              "TypedefAnnoCheckerPass: in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidConstInvokeStatic:()I , the "
              "int value 5 does not have the typedef annotation  "
              "Linteg/TestIntDef; attached to it. Check that the value is "
              "annotated and exists in its typedef annotation class. This "
              "error occured while trying to invoke the method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testIntAnnoInvokeStatic:(I)I .");
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
              "TypedefAnnoCheckerPass: in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidConstInvokeStatic2:()I , the "
              "int value 5 does not have the typedef annotation  "
              "Linteg/TestIntDef; attached to it. Check that the value is "
              "annotated and exists in its typedef annotation class. This "
              "error occured while trying to invoke the method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testIntAnnoInvokeStatic:(I)I .");
}

TEST_F(TypedefAnnoCheckerTest, TestMultipleBlocksInt) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testMultipleBlocksInt:(I)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
              "TypedefAnnoCheckerPass: in the "
              "method Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testInvalidMultipleBlocksString:(Ljava/"
              "lang/String;)Ljava/lang/String; , the source of the value with "
              "annotation  Linteg/TestStringDef; is produced by invoking an "
              "unresolveable callee, so the value safety is not guaranteed.");
}

TEST_F(TypedefAnnoCheckerTest, TestNonConstInt) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testNonConstInt:(I)I")
                    ->as_def();
  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
      "Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testNonConstInt:(I)I "
      "does not guarantee value safety for the value with typedef annotation  "
      "Linteg/TestIntDef; . Check that this value does not change within the "
      "method");
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
      "facebook/redextest/I;)Lcom/facebook/redextest/I; .");
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
              "TypedefAnnoCheckerPass: in method "
              "Lcom/facebook/redextest/"
              "TypedefAnnoCheckerTest;.testJoiningTwoAnnotations:(Ljava/lang/"
              "String;Ljava/lang/String;)Ljava/lang/String; , one of the "
              "parameters needs to have the typedef annotation  "
              "Linteg/TestStringDef; attached to it. Check that the value is "
              "annotated and exists in the typedef annotation class");
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

TEST_F(TypedefAnnoCheckerTest, TestIfElse) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testIfElse:()I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

TEST_F(TypedefAnnoCheckerTest, TestIfElseParam) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testIfElseParam:(Z)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

TEST_F(TypedefAnnoCheckerTest, TestIfElseString) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testIfElseString:(Z)Ljava/lang/"
                    "String;")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

TEST_F(TypedefAnnoCheckerTest, TestXORIfElse) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testXORIfElse:(Z)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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

TEST_F(TypedefAnnoCheckerTest, TestXORIfElseZero) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testXORIfElseZero:()I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  auto& cfg = code->cfg();

  type_inference::TypeInference inference(cfg);
  inference.run(method);

  auto block = cfg.blocks().at(0);
  auto env = inference.get_entry_state_at(block);
  for (auto& mie : InstructionIterable(block)) {
    auto insn = mie.insn;
    if (insn->opcode() == OPCODE_XOR_INT_LIT) {
      EXPECT_EQ(env.get_type(insn->src(0)),
                type_inference::TypeDomain(IRType::ZERO));
    }
    inference.analyze_instruction(insn, &env);
  }

  ConcurrentMap<const DexClass*, std::unordered_set<const DexString*>>
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
