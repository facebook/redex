/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.proguard;

public class Sub extends TheSuper {
  public int pubSubField;
  private int privSubField;

  public void pubSubMethod() { }
  public void pubSubMethod(int x) { }
  private void privSubMethod() { }
  private void privSubMethod(int x) { }
}
