/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.Random;

class CopyPropagationTest {

  public static final Object a = new Object();
  void remove(int _) {
    Object i = a;
    i = a;
  }

  public static Object b = new Object();
  void no_remove() {
    Object i = b;
    i = b; // should not be deleted because b is not final
  }

  public static final long c = new Random().nextLong();
  void remove(long _) {
    long l = c;
    l = c;
  }

  public static final double d = new Random().nextDouble();
  void remove(double _) {
    double n = d;
    int i = 0;
    if (new Random().nextInt() == 1) {
      i = 1;
    } else {
      i = 2;
    }
    n = d;
  }

  public static volatile int e = new Random().nextInt();
  void no_remove(int _) {
    int i = e;
    i = e;
  }
}

class Foo {
}
