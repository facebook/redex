/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This Java class is used as a simple container for dynamically generated
 * methods.
 */

package com.facebook.redextest;

import static org.assertj.core.api.Assertions.*;

import java.lang.reflect.Modifier;
import java.lang.reflect.Method;
import org.junit.Test;
import com.facebook.annotations.*;

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

class B {
  public int fa = 0;
  // staticizable using this
  @DoNotDevirtualize
  public int foo() {
    return 42 + this.fa;
  }
  // staticizable not using this
  @DoNotDevirtualize
  public int baz() {
    return 42;
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


public class MethodDevirtualizationAnonTest {
  @Test
  public void testCallingDevirtualizedMethods() {
    assertThat(new C().callADotFoo()).isEqualTo(42);
    assertThat(new C().callBDotFoo()).isEqualTo(42);
  }
}
