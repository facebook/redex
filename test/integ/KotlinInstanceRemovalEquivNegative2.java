/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Multiple use for INSTANCE
final class KotlinInstanceRemovalEquivNegative2 {
  public final static KotlinInstanceRemovalEquivNegative2 INSTANCE;

  static {
    INSTANCE = new KotlinInstanceRemovalEquivNegative2();
  }
  void Foo() {
    INSTANCE.print();
  }
  void print () {
    System.out.print ("Hello");
  }

  void bar() {
    INSTANCE.print();
  }
}

