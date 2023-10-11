/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import integ.TestIntDef;
import integ.TestStringDef;

@interface NotSafeAnno {}

interface I {
  int get();
}

public class TypedefAnnoCheckerTest {

  @TestIntDef int int_field;
  @TestStringDef int wrong_anno_field;
  @TestStringDef String str_field;

  void testIntField(@TestIntDef int val) {
    int_field = val;
  }

  void testWrongIntField(@TestIntDef int val) {
    wrong_anno_field = val;
  }

  void testStringField(@TestStringDef String val) {
    str_field = val;
  }

  static @NotSafeAnno @TestIntDef int testValidIntAnnoReturn(@NotSafeAnno @TestIntDef int val) {
    return val;
  }

  static @NotSafeAnno @TestStringDef String testValidStrAnnoReturn(@NotSafeAnno @TestStringDef String val) {
    return val;
  }

  static @TestIntDef int testIntAnnoInvokeStatic(@TestIntDef int val) {
    return testValidIntAnnoReturn(val);
  }

  static @TestStringDef String testStringAnnoInvokeStatic(@TestStringDef String val) {
    return testValidStrAnnoReturn(val);
  }

  static @TestIntDef String testWrongAnnotationReturned(@TestStringDef String val) {
    return val;
  }

  static @TestIntDef int testWrongAnnoInvokeStatic(@TestStringDef int val) {
    return testValidIntAnnoReturn(val);
  }

  static @NotSafeAnno @TestIntDef int testIrrelevantAnnos(@TestIntDef int val) {
    return val;
  }

  static @TestIntDef int testConstReturn() {
    @TestIntDef int val = TestIntDef.FOUR;
    return val;
  }

  static @TestIntDef int testInvalidConstReturn() {
    int val = 5;
    return val;
  }

  static @TestIntDef int testInvalidConstReturn2() {
    @TestIntDef int val = 5;
    return val;
  }

  static @TestStringDef String testConstStrReturn() {
   String val = "one";
    return val;
  }

  static @TestStringDef String testInvalidConstStrReturn() {
    @TestStringDef String val = "five";
    return val;
  }

  static @TestIntDef int testInvalidConstInvokeStatic() {
    int val = 5;
    return testIntAnnoInvokeStatic(val);
  }

  static @TestIntDef int testInvalidConstInvokeStatic2() {
    @TestIntDef int val = 5;
    return testIntAnnoInvokeStatic(val);
  }

  static @TestIntDef int testMultipleBlocksInt(@TestIntDef int val) {
    if (val > TestIntDef.TWO) {
      return val;
    }
    return TestIntDef.FOUR;
  }

  static @TestStringDef String testMultipleBlocksString(@TestStringDef String val) {
    if (val.equals(TestStringDef.THREE)) {
      return val;
    }
    return TestStringDef.ONE;
  }

  static @TestStringDef String testInvalidMultipleBlocksString(@TestStringDef String val) {
    if (val.equals(TestStringDef.THREE)) {
      val = val.concat("five");
      return val;
    }
    return TestStringDef.ONE;
  }

  static @TestIntDef int testNonConstInt(@TestIntDef int val) {
    val += 2;
    return val;
  }

  static @TestIntDef I testInvalidType(@TestIntDef I val) {
    return val;
  }

  static @TestStringDef String testJoiningTwoAnnotations(@TestStringDef String val, @TestIntDef String val2) {
    int flag = 1;
    @TestStringDef String s = val;
    if (flag == 1) {
      s = val;
    } else {
      s = val2;
    }
    return s;
  }

  static @TestStringDef String testJoiningTwoAnnotations2(@TestStringDef String val, @TestStringDef String val2) {
    int flag = 1;
    @TestStringDef String s = val;
    if (flag == 1) {
      s = val;
    } else {
      s = val2;
    }
    return s;
  }

  static @TestIntDef int testReassigningInt(@TestStringDef int val, @TestIntDef int val2) {
    val = val2;
    return val;
  }

  static @TestIntDef int testIfElse() {
    boolean flag = true;
    int res = flag ? TestIntDef.ONE : TestIntDef.ZERO;
    return res;
  }

  static @TestIntDef int testIfElseParam(boolean flag) {
    int res = flag ? TestIntDef.ONE : TestIntDef.ZERO;
    return res;
  }

  static @TestStringDef String testIfElseString(boolean flag) {
    String res = flag ? TestStringDef.ONE : TestStringDef.TWO;
    return res;
  }

  static @TestIntDef int testXORIfElse(boolean flag) {
    int res = flag ? TestIntDef.ZERO : TestIntDef.ONE;
    return res;
  }

  static @TestIntDef int testXORIfElseZero() {
    boolean flag = false;
    int res = flag ? TestIntDef.ZERO : TestIntDef.ONE;
    return res;
  }
}
