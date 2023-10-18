/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class ObjectSensitiveDceTest {
  public static void basic() {
    Useless useless = new Useless();
    useless.foo();
  }

  public static void recursive() {
    Useless useless = new Useless();
    useless.recursive_foo(100);
  }

  public static void mutually_recursive() {
    Useless useless = new Useless();
    useless.mutually_recursive_foo(100);
  }

  public static void invoke_super() {
    UselessDerived useless = new UselessDerived();
    useless.foo();
  }

  public static void non_termination() {
    Useless useless = new Useless();
    useless.non_terminating_foo();
  }

  public static void clinit_with_side_effects() {
    UselessWithClInitWithSideEffects useless = new UselessWithClInitWithSideEffects();
    useless.foo();
  }

  public static void method_needing_init_class() {
    UselessWithMethodNeedingInitClass useless = new UselessWithMethodNeedingInitClass();
    useless.foo();
  }
}

class Useless {
  int F;
  public Useless() {}
  public void foo() {
    F = 42;
  }
  public void non_terminating_foo() {
    while (true) {}
  }
  public void recursive_foo(int x) {
    if (x<=0) {
      F = 42;
    } else {
      recursive_foo(x-1);
    }
  }
  public void mutually_recursive_foo(int x) {
    if (x<=0) {
      F = 42;
    } else {
      mutually_recursive_bar(x-1);
    }
  }
  public void mutually_recursive_bar(int x) {
    if (x<=0) {
      F = 42;
    } else {
      mutually_recursive_foo(x-1);
    }
  }
}

class UselessBase {
  int F;
  public UselessBase() {}
  public void foo() {
    F = 42;
  }
}

class UselessDerived extends UselessBase {
  public UselessDerived() {}
  public void foo() {
    super.foo();
  }
}

class UselessWithClInitWithSideEffects {
  int F;
  public UselessWithClInitWithSideEffects() {}
  static {
    try {
      System.loadLibrary("boo"); // side effect
    } catch (Throwable t) { }
  }
  public void foo() {
    F = 42;
  }
}

class UselessWithMethodNeedingInitClass {
  int F;
  public UselessWithMethodNeedingInitClass() {}
  public void foo() {
    F = new UselessWithClInitWithSideEffects().F;
  }
}
