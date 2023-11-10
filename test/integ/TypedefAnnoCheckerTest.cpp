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

  void gather_typedef_values(TypedefAnnoCheckerPass pass,
                             DexClass* cls,
                             StrDefConstants& strdef_constants,
                             IntDefConstants& intdef_constants) {
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
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: The method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testWrongAnnotationReturned:(Ljava/lang/String;)Ljava/lang/String;\n\
 has an annotation Linteg/TestIntDef;\n\
 in its method signature, but the returned value contains the annotation \n\
 Linteg/TestStringDef; instead.\n\
 failed instruction: RETURN_OBJECT v0\n\n");
}

TEST_F(TypedefAnnoCheckerTest, TestWrongAnnoInvokeStatic) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testWrongAnnoInvokeStatic:(I)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: while invoking Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testValidIntAnnoReturn:(I)I\n\
 in method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testWrongAnnoInvokeStatic:(I)I\n\
 parameter 0 has the annotation  Linteg/TestStringDef;\n\
 but the method expects the annotation to be Linteg/TestIntDef;.\n\
 failed instruction: INVOKE_STATIC v1, Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testValidIntAnnoReturn:(I)I\n\n");
}

TEST_F(TypedefAnnoCheckerTest, TestIntField) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testIntField:(I)V")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());

  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: The method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testWrongIntField:(I)V\n\
 assigned a field wrong_anno_field\n\
 with annotation  Linteg/TestStringDef;\n\
 to a value with annotation  Linteg/TestIntDef;.\n\
 failed instruction: IPUT v1, v0, Lcom/facebook/redextest/TypedefAnnoCheckerTest;.wrong_anno_field:I\n\n");
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
  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: in method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testInvalidConstReturn:()I\n\
 the int value 5 does not have the typedef annotation \n\
 Linteg/TestIntDef; attached to it. \n\
 Check that the value is annotated and exists in its typedef annotation class.\n\
 failed instruction: CONST v0, 5\n\
 Error caught when returning the faulty value\n\n");
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: in method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testInvalidConstReturn2:()I\n\
 the int value 5 does not have the typedef annotation \n\
 Linteg/TestIntDef; attached to it. \n\
 Check that the value is annotated and exists in its typedef annotation class.\n\
 failed instruction: CONST v0, 5\n\
 Error caught when returning the faulty value\n\n");
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: in method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testInvalidConstStrReturn:()Ljava/lang/String;\n\
 the string value five does not have the typedef annotation \n\
 Linteg/TestStringDef; attached to it. \n\
 Check that the value is annotated and exists in the typedef annotation class.\n\
 failed instruction: CONST_STRING \"five\"\n\
 Error caught when returning the faulty value\n\n");
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: in method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testInvalidConstInvokeStatic:()I\n\
 the int value 5 does not have the typedef annotation \n\
 Linteg/TestIntDef; attached to it. \n\
 Check that the value is annotated and exists in its typedef annotation class.\n\
 failed instruction: CONST v0, 5\n\
 Error invoking Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testIntAnnoInvokeStatic:(I)I\n\
 Incorrect parameter's index: 0\n\n");
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: in method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testInvalidConstInvokeStatic2:()I\n\
 the int value 5 does not have the typedef annotation \n\
 Linteg/TestIntDef; attached to it. \n\
 Check that the value is annotated and exists in its typedef annotation class.\n\
 failed instruction: CONST v0, 5\n\
 Error invoking Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testIntAnnoInvokeStatic:(I)I\n\
 Incorrect parameter's index: 0\n\n");
}

TEST_F(TypedefAnnoCheckerTest, TestMultipleBlocksInt) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testMultipleBlocksInt:(I)I")
                    ->as_def();

  IRCode* code = method->get_code();
  code->build_cfg();

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: in the method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testInvalidMultipleBlocksString:(Ljava/lang/String;)Ljava/lang/String;\n\
 the source of the value with annotation  Linteg/TestStringDef;\n\
 is produced by invoking an unresolveable callee, so the value safety is not guaranteed.\n\
 failed instruction: INVOKE_VIRTUAL v1, v0, Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;\n\
 Error caught when returning the faulty value\n\n");
}

TEST_F(TypedefAnnoCheckerTest, TestNonConstInt) {
  auto scope = build_class_scope(stores);
  auto method = DexMethod::get_method(
                    "Lcom/facebook/redextest/"
                    "TypedefAnnoCheckerTest;.testNonConstInt:(I)I")
                    ->as_def();
  IRCode* code = method->get_code();
  code->build_cfg();

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: the method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testNonConstInt:(I)I\n\
 does not guarantee value safety for the value with typedef annotation  Linteg/TestIntDef; .\n\
 Check that this value does not change within the method\n\
 failed instruction: ADD_INT_LIT v0, v0, 2\n\
 Error caught when returning the faulty value\n\n");
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(checker.error(),
            "TypedefAnnoCheckerPass: the annotation  Linteg/TestIntDef;\n\
 annotates a value with an incompatible type or a non-constant value in method\n\
 Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testInvalidType:(Lcom/facebook/redextest/I;)Lcom/facebook/redextest/I; .\n\
 failed instruction: RETURN_OBJECT v0\n\n");
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, get_config());
  checker.run(method);
  code->clear_cfg();

  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: in method Lcom/facebook/redextest/TypedefAnnoCheckerTest;.testJoiningTwoAnnotations:(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;\n\
 one of the parameters needs to have the typedef annotation  Linteg/TestStringDef;\n\
 attached to it. Check that the value is annotated and exists in the typedef annotation class.\n\
 failed instruction: IOPCODE_LOAD_PARAM_OBJECT v4\n\
 Error caught when returning the faulty value\n\n");
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
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

TEST_F(TypedefAnnoCheckerTest, testSynthAccessor) {
  auto scope = build_class_scope(stores);
  auto accessor = DexMethod::get_method(
                      "Lcom/facebook/redextest/"
                      "TypedefAnnoCheckerKtTest;.access$takesStrConst:(Lcom/"
                      "facebook/redextest/TypedefAnnoCheckerKtTest;Ljava/lang/"
                      "String;)Ljava/lang/String;")
                      ->as_def();

  EXPECT_TRUE(accessor != nullptr);
  IRCode* code = accessor->get_code();
  code->build_cfg();
  // auto& cfg = code->cfg();

  StrDefConstants strdef_constants;
  IntDefConstants intdef_constants;
  TypedefAnnoCheckerPass pass = TypedefAnnoCheckerPass(get_config());
  for (auto cls : scope) {
    gather_typedef_values(pass, cls, strdef_constants, intdef_constants);
  }

  auto config = get_config();
  TypedefAnnoChecker checker =
      TypedefAnnoChecker(strdef_constants, intdef_constants, config);
  checker.run(accessor);

  // Without patching the accessor, the checker will fail.
  EXPECT_FALSE(checker.complete());
  EXPECT_EQ(
      checker.error(),
      "TypedefAnnoCheckerPass: in method Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.access$takesStrConst:(Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;Ljava/lang/String;)Ljava/lang/String;\n\
 one of the parameters needs to have the typedef annotation  Linteg/TestStringDef;\n\
 attached to it. Check that the value is annotated and exists in the typedef annotation class.\n\
 failed instruction: IOPCODE_LOAD_PARAM_OBJECT v2\n\
 Error invoking Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.takesStrConst:(Ljava/lang/String;)Ljava/lang/String;\n\
 Incorrect parameter's index: 1\n\n");

  SynthAccessorPatcher patcher(config);
  patcher.run(scope);

  TypedefAnnoChecker checker2 =
      TypedefAnnoChecker(strdef_constants, intdef_constants, config);
  checker2.run(accessor);
  // After patching the accessor, the checker should succeed.
  EXPECT_TRUE(checker2.complete());

  auto accessor_caller = DexMethod::get_method(
                             "Lcom/facebook/redextest/"
                             "TypedefAnnoCheckerKtTest$testSynthAccessor$lmd$1;"
                             ".invoke:()Ljava/lang/String;")
                             ->as_def();
  EXPECT_TRUE(accessor_caller != nullptr);
  code = accessor_caller->get_code();
  code->build_cfg();

  TypedefAnnoChecker checker3 =
      TypedefAnnoChecker(strdef_constants, intdef_constants, config);
  checker3.run(accessor_caller);
  // The caller of the accessor has the actual violation.
  EXPECT_FALSE(checker3.complete());
  EXPECT_EQ(
      checker3.error(),
      "TypedefAnnoCheckerPass: in method Lcom/facebook/redextest/TypedefAnnoCheckerKtTest$testSynthAccessor$lmd$1;.invoke:()Ljava/lang/String;\n\
 the string value liu does not have the typedef annotation \n\
 Linteg/TestStringDef; attached to it. \n\
 Check that the value is annotated and exists in the typedef annotation class.\n\
 failed instruction: CONST_STRING \"liu\"\n\
 Error invoking Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;.access$takesStrConst:(Lcom/facebook/redextest/TypedefAnnoCheckerKtTest;Ljava/lang/String;)Ljava/lang/String;\n\
 Incorrect parameter's index: 1\n\n");
}
