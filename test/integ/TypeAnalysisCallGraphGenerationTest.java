/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

interface I {
  String getName();
}

class Base implements I {
  int getVal() { return 0; }
  @Override
  public String getName() { return "Base"; }
}

class SubOne extends Base implements I {
  @Override
  int getVal() {
    return 1;
  }
  @Override
  public String getName() {
    return "SubOne";
  }
}

class SubTwo extends Base implements I {
  @Override
  int getVal() {
    return 2;
  }
  @Override
  public String getName() {
    return "SubTwo";
  }
}

public class TypeAnalysisCallGraphGenerationTest {

  final Base mBase;
  final I mIntf;

  TypeAnalysisCallGraphGenerationTest() {
    mBase = new SubTwo();
    mIntf = new SubTwo();
  }

  int baseArg(Base b) {
    return b.getVal();
  }

  Base baseReturn(Base b) {
    if (b != null) {
      return b;
    }
    return null;
  }

  String intfArg(I i) { return i.getName(); }

  I intfReturn(I i) {
    if (i != null) {
      return i;
    }
    return null;
  }

  int baseField() { return mBase.getVal(); }

  String intfField() { return mIntf.getName(); }

  static void main() {
    Base b = new Base();
    b.getVal();

    TypeAnalysisCallGraphGenerationTest t =
        new TypeAnalysisCallGraphGenerationTest();
    SubOne s1 = new SubOne();
    b = t.baseReturn(s1);
    t.baseArg(b);

    I i = t.intfReturn(s1);
    t.intfArg(i);

    t.baseField();
    t.intfField();
  }
}
