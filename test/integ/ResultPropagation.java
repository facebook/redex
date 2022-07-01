/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

interface I {
  public int foo(boolean b, int i, int j);
}

class A implements I {
  public int foo(boolean b, int i, int j) {
    // note the multiple potential result value flows
    return b ? i : j;
  }
}

abstract class B implements I {
  public abstract int foo(boolean b, int i, int j);
}

abstract class B1 extends B {
  public int foo(boolean b, int i, int j) {
    return i;
  }
}

abstract class B2 extends B {
  public int foo(boolean b, int i, int j) {
    return i;
  }
}

class C {
  private Object getThis() {
    return (C) this;
  }

  public C setSomething(int something) {
    return (C) getThis();
  }
}

class D {
  public D strongCast(Object value) {
    return (D) value;
  }
}

public class ResultPropagation {
  public class Test1 {
    int returns_none(I receiver, boolean b, int i, int j) {
      return receiver.foo(b, i, j);
    }
  }

  public class Test2 {
    int returns_none(A receiver, boolean b, int i, int j) {
      return receiver.foo(b, i, j);
    }
  }

  public class Test3 {
    int returns_3(B1 receiver, boolean b, int i, int j) {
      return receiver.foo(b, i, j);
    }
  }

  public class Test4 {
    int returns_3(B receiver, boolean b, int i, int j) {
      return receiver.foo(b, i, j);
    }
  }

  public class Test5 {
    Object returns_1(C receiver, int something) {
      // this requires analysis through a few hops, really exercising the
      // fixed point computation
      return receiver.setSomething(something);
    }
  }

  public class Test6 {
    D returns_none(D receiver, Object value) {
      // leveraging knowledge about the dataflow would produce unverifiable
      // code, so the transformation is not applied by default
      return receiver.strongCast(value);
    }
  }
}
