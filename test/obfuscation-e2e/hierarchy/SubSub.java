/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

public class SubSub extends SubImpl {
  public int pubSubsubField;
  private int privSubsubField;
  private int privSuperField;

  public SubSub() { }
  public SubSub(int x) { }

  @Override
  public void pubTheSuperMethod() {}

  public void pubSubsubMethod() {}
  private void privSubsubMethod() {}
}
