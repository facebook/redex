/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import java.lang.Runnable;

import com.facebook.redex.annotations.UsesNames;
import com.facebook.redex.annotations.UsesNamesTransitive;
import com.facebook.annotations.OkToExtend;

class Callsite {
  void use(@UsesNames ClassA a) {
  }

  void use2(@UsesNames InterB b, @UsesNames SubC c) {
  }

  void uses3(@UsesNamesTransitive ClassD d) {
  }

  // Check no strange behavior for external class
  void uses4(@UsesNamesTransitive Object o) {
  }

  // Check no strange behavior for primitive type
  void uses4(@UsesNamesTransitive int x) {
  }
}

class ClassA {
  int aField1;
  FieldAType aField2;
  int method1() { return 2; }
  static int method0() { return 6; }
}

@OkToExtend
class SubA extends ClassA {
  int aField2;
  int method2() { return 1; }
}

class FieldAType {

}

interface InterB {
  abstract public int method3();
}

class SubB implements InterB {
  int bField4;
  public int method3() { return 3; }
}

class C {
  int field6;
  int method6() { return 3; }
}

@OkToExtend
class SubC extends C {
  int field7;
  int method7() { return 3; }
}

class ClassD {
  FieldDType field1;
  Runnable field2;
  int method9() { return 3; }
}

class FieldDType {
  int field;
  ClassD d2; // D->FieldDType->D is a cycle
  int method() { return 3; }
}

class FieldDType2 implements Runnable {
  public void run() {
  }
}

class NotUsed {
  int field5;
  public int method5() { return 3; }
}
