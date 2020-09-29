/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class MaxDepthAnalysisTest {
  static void a0() {}

  static void a1() {
    a0();
  }

  static void a2() {
    a1();
  }

  static void a3() {
    a2();
    a1();
  }

  static void a4() {
    a3();
  }

  static void a5() {
    a4();
  }

  static void a6() {
    a5();
    a3();
  }

  static void a7() {
    a6();
    a2();
    a0();
  }

  static void a8() {
    a1();
    a7();
    a7();
    a0();
  }

  static void recursive1() {
    recursive2();
  }

  static void recursive2() {
    recursive1();
  }
}
