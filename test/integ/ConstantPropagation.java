/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

/**
 * This Java class is used to test the constant propagation and conditional
 * pruning optimization that needs to be written.
 * Currently, The test uses localDCE, Dellnit and RemoveEmptyClasses.
 */

package com.facebook.redextest;

class Alpha {
  public static int theAnswer() {
    boolean x = false;
    if (x) {
      return 42;
    } else {
      return 32;
    }
  }
}

class Beta {
  private final boolean gammaConfig = false;
  public boolean getConfig() {
    return gammaConfig;
  }
}

class Propagation {
  // Senario 1: Constant in if statement
  // Test whether class Alpha is removed
  public static int propagation_1() {
    int y;
    boolean x;
    x = false;
    if (x) {
      y = Alpha.theAnswer();
    } else {
      y = 42;
    }
    return y;
  }

  // Senario 2: Method returning a constant
  // Test whether class Alpha is removed
  public static int propagation_2() {
    int y;
    boolean x;
    x = false;
    if (x) {
      y = 32;
    } else {
      y = Alpha.theAnswer();
    }
    return y;
  }

  // Senario 3: Constant from field, propagate across parameters and return value
  // Test whether class Gamma and Alpha are removed
  public static int propagation_3() {
    int y;
    Beta b = new Beta();
    if (b.getConfig()) {
      y = Alpha.theAnswer();
    } else {
      y = 35;
    }
    return y;
  }
}

public class ConstantPropagation {
  public static void main(String[] args) {
    Propagation p = new Propagation();
    System.out.println(p.propagation_1());
    System.out.println(p.propagation_2());
    System.out.println(p.propagation_3());
  }
}
