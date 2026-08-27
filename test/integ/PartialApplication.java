/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class PartialApplication {
  static class Callees {
    static void foo(int a, int b, int c, int d, int e, int f, int g, int h) {
    }

    static int bar(short a, char b, Integer c, String s, int e, int f, int g, int h) {
      return 42;
    }
  }

  public static void call_foo1() {
    Callees.foo(0, 1, 2, 3, 4, 5, 6, 7);
  }
  public static void call_foo2() {
    Callees.foo(0, 1, 2, 3, 4, 5, 6, 7);
  }
  public static void call_foo3() {
    Callees.foo(0, 1, 2, 3, 4, 5, 6, 7);
  }
  public static void call_foo4() {
    Callees.foo(0, 1, 2, 3, 4, 5, 6, 7);
  }
  public static void call_foo5() {
    Callees.foo(0, 1, 2, 3, 4, 5, 6, 7);
  }
  public static void call_foo6() {
    Callees.foo(0, 1, 2, 3, 4, 5, 6, 7);
  }
  public static void call_foo7() {
    Callees.foo(0, 1, 2, 3, 4, 5, 6, 7);
  }

  public static void call_bar1() {
    Callees.bar((short)0, 'A', Integer.valueOf(1), null, 3, 4, 5, 6);
  }
  public static void call_bar2() {
    Callees.bar((short)0, 'A', Integer.valueOf(1), null, 3, 4, 5, 6);
  }
  public static void call_bar3() {
    Callees.bar((short)0, 'A', Integer.valueOf(1), null, 3, 4, 5, 6);
  }
  public static void call_bar4() {
    Callees.bar((short)0, 'A', Integer.valueOf(1), null, 3, 4, 5, 6);
  }
  public static void call_bar5() {
    Callees.bar((short)0, 'A', Integer.valueOf(1), null, 3, 4, 5, 6);
  }

  static class MoreCallees {
    public int baz(int a, int b, int c, int d, int e, int f, int g, int h) {
      return 42;
    }
  }

  public static int call_baz1() {
    MoreCallees mc = new MoreCallees();
    return mc.baz(100, 1111, 2222, 3333, 4444, 5555, 6666, 200);
  }

  public static int call_baz2() {
    MoreCallees mc = new MoreCallees();
    return mc.baz(101, 1111, 2222, 3333, 4444, 5555, 6666, 201);
  }

  public static int call_baz3() {
    MoreCallees mc = new MoreCallees();
    return mc.baz(102, 1111, 2222, 3333, 4444, 5555, 6666, 202);
  }

  public static int call_baz4() {
    MoreCallees mc = new MoreCallees();
    return mc.baz(103, 1111, 2222, 3333, 4444, 5555, 6666, 203);
  }

  public static int call_baz5() {
    MoreCallees mc = new MoreCallees();
    return mc.baz(104, 1111, 2222, 3333, 4444, 5555, 6666, 204);
  }

  public static int call_baz6() {
    MoreCallees mc = new MoreCallees();
    return mc.baz(105, 1111, 2222, 3333, 4444, 5555, 6666, 205);
  }

  private static native void consume(int value);

  static class RegressionCallee {
    static void target(int a, int b, int c) {
    }
  }

  public static void call_regression_a(int b, int c) {
    int a = 1;
    consume(a);
    RegressionCallee.target(a, b, c);
  }

  public static void call_regression_ab1(int c) {
    int b = 2;
    consume(b);
    RegressionCallee.target(1, b, c);
  }

  public static void call_regression_ab2(int c) {
    int b = 2;
    consume(b);
    RegressionCallee.target(1, b, c);
  }

  public static void call_regression_c1(int a, int b) {
    RegressionCallee.target(a, b, 3);
  }

  public static void call_regression_c2(int a, int b) {
    RegressionCallee.target(a, b, 3);
  }

  public static void call_regression_bc1(int a) {
    int b = 2;
    consume(b);
    RegressionCallee.target(a, b, 3);
  }

  public static void call_regression_bc2(int a) {
    int b = 2;
    consume(b);
    RegressionCallee.target(a, b, 3);
  }

  public static void call_regression_abc1() {
    int a = 1;
    consume(a);
    RegressionCallee.target(a, 2, 3);
  }

  public static void call_regression_abc2() {
    int a = 1;
    consume(a);
    RegressionCallee.target(a, 2, 3);
  }

  public static void call_regression_abc3() {
    int a = 1;
    int b = 2;
    consume(a);
    consume(b);
    RegressionCallee.target(a, b, 3);
  }
}
