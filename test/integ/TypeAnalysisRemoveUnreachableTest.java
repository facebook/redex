/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class Base1 {
  int foo() { return 1; }
}
class Sub1 extends Base1 {
  static Sub1 createSub1() { return new Sub1(); }
  @Override
  int foo() {
    return 2;
  }
}
class SubSub1 extends Sub1 {
  static SubSub1 createSubSub1() { return new SubSub1(); }
  @Override
  int foo() {
    return 3;
  }
}

interface Intf1 {
  int bar();
}
class Impl1 implements Intf1 {
  static Intf1 createImpl1() { return new Impl1(); }
  @Override
  public int bar() {
    return 1;
  }
}
class Impl2 implements Intf1 {
  static Intf1 createImpl2() { return new Impl2(); }
  @Override
  public int bar() {
    return 2;
  }
}

public class TypeAnalysisRemoveUnreachableTest {
  public void typeAnalysisRMUTest1() {
    Base1 b = Sub1.createSub1();
    b.foo();
  }

  public void typeAnalysisRMUTest2() {
    Intf1 i = Impl1.createImpl1();
    i.bar();
    i = Impl2.createImpl2();
  }

  public void typeAnalysisRMUTest3() {
    Base1 b = Sub1.createSub1();
    SubSub1.createSubSub1();
    b.foo();
  }
}
