/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class SourceBlocksTest {
  String mHello = null;

  public void foo() {
    System.out.println("Hello");
  }

  public void bar() {
    baz("World");
  }

  private void baz(String s) {
    mHello = s;
  }

  public void bazz() {
    bar();
  }
}
