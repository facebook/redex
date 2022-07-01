/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;
public class StripDebugInfo {

  // Have a function with no arguments to ensure strip parameter names doesn't
  // mess anything up.
  public StripDebugInfo() {
    String m = "m";
    message = "StripDebugInfo()";
    System.out.println(message);
  }
  // Have a function with one argument to ensure strip parameter names works.
  public StripDebugInfo(int v) {
    String m = "m";
    System.out.println("StripDebugInfo(int v)");
  }
  // Have a function with more than one argument to ensure strip parameter
  // names works.
  public StripDebugInfo(int v, int w) {
    String m = "m";
    System.out.println("StripDebugInfo(int v, int w)");
  }

  // Locals with varying lifetimes to verify we can strip locals.
  public void TestVariables(int i) {
    String m = "m";
    if ((i % 1) == 0) {
      String n = "n";
      System.out.println(n);
    } else {
      String o = "o";
      System.out.println(o);
    }
    System.out.println(m);
  }
  public String message;
}
