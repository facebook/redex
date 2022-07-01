/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

public class Theta {

  public class A implements Eta.T1 {
    public int wombat() { return 42; }
    public int numbat() { return 1066; }
  }

  public class B extends A {}

}
