/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redextest;

public class InstrumentTarget {
  public static void func1() {
    System.out.println("func1");
  }

  public static void func2(int k) {
    System.out.println("func2 " + k);
  }

  public int func3(int a, int b) {
    System.out.println("func2: " + a + b);
    return a + b;
  }

  public static void main(String[] args) {
    InstrumentTarget target = new InstrumentTarget();
    InstrumentTarget.func1();
    InstrumentTarget.func2(42);
    System.out.println("Result: " + target.func3(4, 42));
  }
}
