/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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

  public static void clinit_with_side_effects() {
    UselessWithClInitWithSideEffects useless = new UselessWithClInitWithSideEffects();
    useless.foo();
  }
}

class Useless {
  int F;
  public Useless() {}
  public void foo() {
    F = 42;
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
