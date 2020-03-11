/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

class Base {
  int getVal() { return 0; }
}

class SubOne extends Base {
  int getVal() { return 1; }
}

class SubTwo extends Base {
  int getVal() { return 2; }
}

class TestA {

  public Base getSubOne() {
    return new SubOne();
  }

  public Base getSubTwo() {
    return new SubTwo();
  }

  public Base passThrough(Base b) {
    int two = 1 + 1;
    return b;
  }

  public int foo() {
    Base one = getSubOne();
    Base two = passThrough(getSubTwo());
    int sum = one.getVal() + two.getVal();
    return sum;
  }
}
