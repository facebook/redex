/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
