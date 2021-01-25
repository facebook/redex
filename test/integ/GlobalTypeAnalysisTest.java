/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class Base {
  int getVal() { return 0; }
}

class SubOne extends Base {
  @Override
  int getVal() {
    return 1;
  }
}

class SubTwo extends Base {
  @Override
  int getVal() {
    return 2;
  }
}

class TestA {

  public Base getSubOne() { return new SubOne(); }

  public Base getSubTwo() { return new SubTwo(); }

  public Base passThrough(Base b) {
    int two = 1 + 1;
    return b;
  }

  public int foo() {
    Base one = getSubOne();
    Base two = passThrough(getSubTwo());
    int sum = one.getVal() + two.getVal();
    return sum;
  }
}

class TestB {

  public String passNull(String a) { return a; }

  public String passString(String a) { return a; }

  public Class passClass(Class cls) { return cls; }

  public String getStringArrayComponent(String[] sa) { return sa[0]; }
  public String[] getNestedStringArrayComponent(String[][] nsa) {
    return nsa[0];
  }

  public static void main() {
    TestB tb = new TestB();
    String a = null;
    tb.passNull(a);
    a = "Yoyo";
    tb.passString(a);
    Class cls = String.class;
    tb.passClass(cls);
    String[] sa = new String[] {a};
    tb.getStringArrayComponent(sa);
    String[][] nsa = new String[][] {sa};
    tb.getNestedStringArrayComponent(nsa);
  }
}

interface Receiver {
  void onChange();
}

class TestC {

  private Receiver mMonitor;

  void change() {
    if (mMonitor != null) {
      remove();
    }

    mMonitor = new Receiver() {
      @Override
      public void onChange() {
        int yo = 1 + 1;
      }
    };
  }

  void remove() {
    if (mMonitor != null) {
      mMonitor = null;
    }
  }

  static void main() {
    TestC t = new TestC();
    t.change();
  }
}

class TestD {

  static class State { Base mVal; }

  static class Base {
    void update(State s) { s.mVal = null; }
  }

  static class Sub extends Base {
    @Override
    void update(State s) {
      s.mVal = this;
    }
  }

  static void main() {
    State s = new State();
    Base b = new Sub();
    b.update(s);
  }
}

class TestE {

  static class Base {
    int foo() { return 0; }
  }

  static class SubOne extends Base {
    int foo() { return 1; }
  }

  static class SubTwo extends Base {
    int foo() { return 2; }
  }

  static class SubThree extends Base {
    int foo() { return 3; }
  }

  Base returnSubTypes(int arg) {
    Base b = null;
    if (arg == 1) {
      b = new SubOne();
    } else if (arg == 2) {
      b = new SubTwo();
    } else if (arg == 3) {
      b = new SubThree();
    }
    return b;
  }

  static void main() {
    TestE t = new TestE();
    Base b = t.returnSubTypes(2);
    b.foo();
  }
}

class TestF {

  int foo() {
    int a = 1;
    int b = 2;
    for (int i = 0; i < 5; i++) {
      b += 1;
    }
    return a;
  }

  static void main() {
    TestF t = new TestF();
    t.foo();
  }
}

class TestG {

  static class Base {}

  Base foo() {
    Base b = new Base();
    Base[] ba = new Base[2];
    for (int i = 0; i < ba.length; i++) {
      ba[i] = b;
    }
    return ba[0];
  }

  Base bar() {
    Base b = new Base();
    Base[] ba = new Base[2];
    int i = 0;
    ba[i] = b;
    ba[++i] = b;
    return ba[1];
  }

  static void main() {
    TestG t = new TestG();
    t.foo();
    t.bar();
  }
}

class TestH {

  static class Base {}
  static class SubOne extends Base {}
  static class SubTwo extends Base {}

  static final Base BASE = initBase();

  Base mBase;

  static Base initBase() { return new Base(); }

  Base returnSubOne() {
    mBase = new SubOne();
    return mBase;
  }

  Base rereturnSubOne() { return returnSubOne(); }

  Base foo() { return rereturnSubOne(); }

  Base returnSubTwo() {
    mBase = new SubTwo();
    return mBase;
  }

  Base rereturnSubTwo() { return returnSubTwo(); }

  Base bar() { return rereturnSubTwo(); }

  Base baz() { return BASE; }

  static void main() {
    final TestH t = new TestH();
    t.foo();
    t.bar();
    t.baz();
  }
}

/*
 * Nullness inference for ifields on class with multiple ctors
 */
class TestI {
  static class Foo {
    String yield() { return "foo"; }
  }

  static class One {
    final Foo m1;
    Foo m2;

    One(final Foo f1, final Foo f2) {
      m1 = f1;
      m2 = f2;
    }
    // Unreachable
    One(final Foo f1) { m1 = f1; }
    String yield() { return m1.yield() + m2.yield(); }
  }

  static class Two {
    final Foo m1;
    Foo m2;

    Two(final Foo f1, final Foo f2) {
      m1 = f1;
      m2 = f2;
    }
    Two(final Foo f1) { m1 = f1; }
    String yield() { return m1.yield() + m2.yield(); }
  }

  static void main() {
    Foo f1 = new Foo();
    Foo f2 = new Foo();
    One one = new One(f1, f2);
    one.yield();

    Two two = new Two(f1, f2);
    two.yield();
    two = new Two(f2);
    two.yield();
  }
}

class TestJ {

  static byte[] createByteArray() { return new byte[5]; }

  static void main() { byte[] ba = createByteArray(); }
}

class TestK {
  static class A {}
  static class B extends A {}

  static class Foo {
    A f;
    Foo() {
      f = new B();
    }
    Foo(Foo other) {
      f = new A();
      other.f = new B();
    }
  }
  static void main() {
    Foo f1 = new Foo();
    Foo f2 = new Foo(f1);
  }
}
