/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class SourceBlocksDedupTest {
  public void someFunc() {
      System.out.println("some");
    }

  public void otherFunc() {
    System.out.println("other");
  }

  public int useSwitch() {
    switch ((int) (Math.random() * 10)) {
    case 0:
      someFunc();
      break;
    case 1:
      someFunc();
      break;
    case 2:
      someFunc();
      break;
    default:
      otherFunc();
      break;
    }
    return 0;
  }

  public int deepestIsNotTheBestCase() {
    int x = 0;
    int y = 1;
    if (x == 0) {
      return x;
    } else if (x == 1) {
      int z = 1;
      x += x;
      x += x;
      return z;
    } else if (x == 2) {
      int z = 2;
      x += x;
      x += x;
      return z;
    } else if (x == 3) {
      int z = 3;
      x += x;
      x += x;
      return z;
    } else if (x == 4) {
      int z = 4;
      x += x;
      x += x;
      return z;
    } else if (x == 5) {
      int z = 5;
      x += x;
      x += x;
      return z;
    } else {
      return x;
    }
  }

  public void dedupThrows() {
    int x = 0;
    if (x == 0) {
      throw new ArithmeticException("throwing");
    } else {
      throw new ArithmeticException("throwing");
    }
  }

  public void simplestCase() {
    int x = 0;
    x *= x;
    if (x == 0) {
      x += x;
    }
    else {
      x *= x;
      x += x;
    }
    return;
  }

  public void postfixDiscardingOne() {
    int x = 0;
    x *= x;
    if (x == 0) {
      if (x == 0) {
        x -= x;
      }
      else {
        x += x;
        x += x;
        x += x;
        x += x;
      }
    }
    else {
      x *= x;
      x *= x;
      x += x;
      x += x;
      x += x;
    }
    x += x;
    return;
  }

  public void identicalSelfLoops() {
    boolean i = true;
    if (i) {
      while (i) {}
    } else {
      while (i) {}
    }
  }
}
