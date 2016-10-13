/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
