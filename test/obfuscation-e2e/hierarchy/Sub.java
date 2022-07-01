/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

class Hello {

}

public class Sub extends TheSuper {
  public int pubSubField;
  //public static int pubStaticSub;

  //private static final int aprivStaticSub = 5;
  //public static final Hello bpubStaticSub = new Hello();

  private int privSubField;

  public void pubSubMethod() { }
  public void pubSubMethod(int x) { }
  private void privSubMethod() { }
  private void privSubMethod(int x) { }
}
