/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class TestClass {
  int finalizable;
  int not_finalizable;
  static int static_finalizable;
  static int static_not_finalizable;
  static int static_not_finalizable2 = 42;
  static {
    static_finalizable = 42;
  }
  TestClass(TestClass other) {
    static_not_finalizable = 42;
    static_not_finalizable2 = 42;
    finalizable = 23;
    finalizable = 42;
    other.not_finalizable = 23;
    not_finalizable = 42;
  }
}
