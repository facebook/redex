/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class CallGraphTest {
  static {
    // root of the graph
    Base b = new Extended();
    callsReturnsInt(b);
    Extended.foo();
    MoreThan5 moreThan5 = new MoreThan5Impl1();
    int get1 = moreThan5.returnNum();
    LessThan5 lessThan5 = new LessThan5Impl3();
    int get3 = lessThan5.returnNum();
  }

  static int callsReturnsInt(Base b) {
    return b.returnsInt();
  }
}

class Base {
  int returnsInt() {
    return 1;
  }

  static int foo() {
    return 2;
  }
}

class Extended extends Base {
  int returnsInt() {
    return 2;
  }
}

class ExtendedExtended extends Extended {
  int returnsInt() {
    return super.returnsInt();
  }
}

interface MoreThan5 {
  public int returnNum();
}

class MoreThan5Impl1 implements MoreThan5 {
  public int returnNum() { return 1; }
}

class MoreThan5Impl2 implements MoreThan5 {
  public int returnNum() { return 2; }
}

class MoreThan5Impl3 implements MoreThan5 {
  public int returnNum() { return 3; }
}

class MoreThan5Impl4 implements MoreThan5 {
  public int returnNum() { return 4; }
}

class MoreThan5Impl5 implements MoreThan5 {
  public int returnNum() { return 5; }
}

class MoreThan5Impl6 implements MoreThan5 {
  public int returnNum() { return 6; }
}

interface LessThan5 {
  public int returnNum();
}

class LessThan5Impl1 implements LessThan5 {
  public int returnNum() { return 1; }
}

class LessThan5Impl2 implements LessThan5 {
  public int returnNum() { return 2; }
}

class LessThan5Impl3 implements LessThan5 {
  public int returnNum() { return 3; }
}

class LessThan5Impl4 implements LessThan5 {
  public int returnNum() {
    PureRefImpl2 pureRef = new PureRefImpl3();
    int get5 = pureRef.returnNum();
    return 4;
  }
}

interface PureRef {
  public int returnNum();
}

abstract class PureRefImpl1 implements PureRef {}

abstract class PureRefImpl2 extends PureRefImpl1 {}

class PureRefImpl3 extends PureRefImpl2 {
  public int returnNum() { return 5; }
}
