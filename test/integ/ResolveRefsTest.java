/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class Base {
  int foo() { return 42; }
}
class Sub extends Base {}

interface I {
  Base getVal();
}
class C implements I {
  @Override
  public Base getVal() {
    return new Sub();
  }
}
