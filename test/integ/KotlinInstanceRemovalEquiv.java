/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

final class KotlinInstanceRemovalEquiv {
  public final static KotlinInstanceRemovalEquiv INSTANCE;
  static {
    INSTANCE = new KotlinInstanceRemovalEquiv();
  }
  public KotlinInstanceRemovalEquiv() {
  }
  void print () {
    System.out.print ("Hello");
  }
  void bar() {
    INSTANCE.print();
  }
}

