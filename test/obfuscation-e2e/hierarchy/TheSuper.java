/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.proguard;

public class TheSuper {
  public int pubSuperField;
  public static int pubStaticSuper = 1;
  public static int pubStaticSuper2 = 2;
  private int privSuperField;

  public void pubTheSuperMethod() { }
  public void privTheSuperMethod() { }
}
