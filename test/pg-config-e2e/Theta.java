/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.proguard;

public class Theta {

  public class A implements Eta.T1 {
    public int wombat() { return 42; }
    public int numbat() { return 1066; }
  }

  public class B extends A {}

}
