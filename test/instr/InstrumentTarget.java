/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
    InstrumentTestClass1.it1Func1();
    if (InstrumentTestClass1.it1Func2()) {
      InstrumentTarget.func1();
    }
    InstrumentTarget.func1();
    InstrumentTarget.func2(42);
    System.out.println("Result: " + target.func3(4, 42));
  }
}

class InstrumentTestClass1 {
  public static void it1Func1() {
    System.out.println(InstrumentTestClass1.class.getSimpleName());
    System.out.println("TestClass1 Func1");
  }

  public static boolean it1Func2() {
    System.out.println("TestClass1 Func2");
    return true;
  }
}
