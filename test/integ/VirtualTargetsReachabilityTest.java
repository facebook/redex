/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class Dead {}

class Base {
  Object foo() { return null; }
}

class Sub extends Base {
  Object foo() { return new Dead(); }
  Object bar() {
    return super.foo();
  }
}

public class VirtualTargetsReachabilityTest {
  public static void root() {
    new Sub().bar();
  }
}
