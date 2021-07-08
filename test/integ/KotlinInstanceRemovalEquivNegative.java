/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// INSTANCE created multiple times

final class KotlinInstanceRemovalEquivNegative {

  public static KotlinInstanceRemovalEquivNegative INSTANCE;

  static {
    INSTANCE = new KotlinInstanceRemovalEquivNegative();
  }
  void Foo() {
    INSTANCE = new KotlinInstanceRemovalEquivNegative();
  }
  
  void print () {
    System.out.print ("Hello");
  }

  void bar() {
    INSTANCE.print();
  }
}

