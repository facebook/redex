/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class CallGraphTest {
  static {
    // root of the graph
    Base b = new Extended();
    callsReturnsInt(b);
    Extended.foo();
  }

  static int callsReturnsInt(Base b) {
    return b.returnsInt();
  }
}

class Base {
  int returnsInt() {
    return 1;
  }

  static int foo() {
    return 2;
  }
}

class Extended extends Base {
  int returnsInt() {
    return 2;
  }
}
