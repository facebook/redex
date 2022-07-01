/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This Java class is used as a simple container for dynamically generated
 * methods.
 */

package com.facebook.redextest;

import static org.fest.assertions.api.Assertions.*;

import java.lang.reflect.Modifier;
import java.lang.reflect.Method;
import org.junit.Test;

class A {
  public int fa = 0;
  // staticizable using this
  public int foo() {
    return 42 + this.fa;
  }
  // staticizable not using this
  public int baz() {
    return 42;
  }
}

class B extends A {
  // staticizable using this
  public int bar() {
    return super.foo();
  }
}

class C {
  static int callADotFoo() {
    A a = new A();
    return a.foo();
  }

  static int callBDotFoo() {
    B b = new B();
    return b.foo();
  }
}

public class MethodDevirtualizationTest {
  @Test
  public void testCallingDevirtualizedMethods() {
    assertThat(new C().callADotFoo()).isEqualTo(42);
    assertThat(new C().callBDotFoo()).isEqualTo(42);
    assertThat(new B().bar()).isEqualTo(42);
  }
}
