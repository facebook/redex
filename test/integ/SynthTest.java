/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class SynthTest {
  public static void main(String[] args) {
    Alpha a = new Alpha(12);
    Alpha.Beta b = a.new Beta();
    System.out.println("2b = " + b.doublex());
    SyntheticConstructor s = new SyntheticConstructor(1);
    SyntheticConstructor.InnerClass i = s.new InnerClass();
    String str = "";
    try {
      java.io.Writer writer = new java.io.StringWriter();
      writer.write("hello");
      Gamma g = new Gamma(writer);
      Gamma.Delta d = g.new Delta();
      str = d.getWriter().toString();
    } catch (java.io.IOException e) {
      str = e.toString();
    }
    System.setProperty("foo", str);
  }
}
