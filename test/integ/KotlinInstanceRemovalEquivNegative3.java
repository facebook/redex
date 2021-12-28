/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Constructor has side effects
final class KotlinInstanceRemovalEquivNegative3 {
  public final static KotlinInstanceRemovalEquivNegative3 INSTANCE;

  public KotlinInstanceRemovalEquivNegative3() {
    System.out.print ("Hello");
  }
  static {
    INSTANCE = new KotlinInstanceRemovalEquivNegative3();
  }
  void print () {
    System.out.print ("Hello");
  }

  void bar() {
    INSTANCE.print();
  }
}

