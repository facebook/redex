/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// No removal should happen because the INSTANCE is not anonymous.
final class KotlinInstanceRemovalNamedEquiv {
  public final static KotlinInstanceRemovalNamedEquiv INSTANCE;
  static {
    INSTANCE = new KotlinInstanceRemovalNamedEquiv();
  }
  public KotlinInstanceRemovalNamedEquiv() {
  }
  void print () {
    System.out.print ("Hello");
  }
  void bar() {
    INSTANCE.print();
  }
}
