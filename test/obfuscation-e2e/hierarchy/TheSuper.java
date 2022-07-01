/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
