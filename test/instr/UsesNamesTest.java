/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import com.facebook.redex.annotations.UsesNames;
import com.facebook.annotations.OkToExtend;

class Callsite {
  void use(@UsesNames ClassA a) {
  }

  void use2(@UsesNames InterB b, @UsesNames SubC c) {
  }
}

class ClassA {
  int aField1;
  int method1() { return 2; }
  static int method0() { return 6; }
}

@OkToExtend
class SubA extends ClassA {
  int aField2;
  int method2() { return 1; }
}

interface InterB {
  abstract public int method3();
}

class SubB implements InterB {
  int bField4;
  public int method3() { return 3; }
}

class Renamed {
  int field5;
  public int method5() { return 3; }
}

class C {
  int field6;
  int method6() { return 3; }
}

class SubC {
  int field7;
  int method7() { return 3; }
}
