/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This Java class is used to test the constant propagation and conditional
 * pruning optimization that needs to be written.
 * Currently, The test uses localDCE, Dellnit and RemoveEmptyClasses.
 */

package com.facebook.redextest;

class Propagation {
  public static int if_false() {
    int y;
    boolean x = false;
    if (x) {
      y = 0;
    } else {
      y = 42;
    }
    return y;
  }

  public static int if_true() {
    int y;
    boolean x = true;
    if (x) {
      y = 32;
    } else {
      y = 0;
    }
    return y;
  }

  public static int if_unknown() {
    int y;
    boolean x = Math.random() > 0.5? true : false;
    if (x) {
      y = 32;
    } else {
      y = 0;
    }
    return y;
  }
}

public class ConstantPropagation {
  public static void main(String[] args) {
    Propagation p = new Propagation();
    System.out.println(p.if_false());
    System.out.println(p.if_true());
  }
}
