/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redextest;

public class EmptyClassesTest {

  public static void main(String[] args) {
    MyBy2Or3 c = new MyBy2Or3(42);
    System.out.println("By 2 = " + c.Double());
    System.out.println("By 3 = " + c.Triple());
    Wombat w = new Wombat();
    System.out.println(w.BadWombat(false));
    MyYesNo yn = new MyYesNo();
    System.out.println(yn.yes());
    NotAnEmptyClass3 nec3 = new NotAnEmptyClass3();
    System.out.println(nec3.yes());
    NotAnEmptyClass5 nec5 = new NotAnEmptyClass5();
    System.out.println(nec5.yes());
    NotAnEmptyClass nec = new NotAnEmptyClass();
    System.out.println(nec.y);
    NotAnEmptyClass2 nec2 = new NotAnEmptyClass2();
    System.out.println(nec2.sy);
    InnerEmpty2 ie2 = new InnerEmpty2();
    System.out.println(ie2.x);
    Extender extender = new Extender();
    System.out.println(extender.x);
  }
}
