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

  static @TestIntDef I testInvalidType(@TestIntDef I val) {
    return val;
  }
}
