/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.proguard;

public class SubImpl extends TheSuper implements Intf {
  public int pubSubImplField;
  private int privSubImplField;

  @Override
  public void pubTheSuperMethod() { }
  // Interface Methods
  public void interfaceMethod() {}
  public void interfaceMethodWithArg(int x) {}
  public void interfaceMethodWithReturn() {}
}
