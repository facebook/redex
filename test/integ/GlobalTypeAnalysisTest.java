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

  public Base getSubOne() {
    return new SubOne();
  }

  public Base getSubTwo() {
    return new SubTwo();
  }

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

  public String passString(String a) { return a + ""; }

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
