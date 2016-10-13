/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.proguard;

import java.util.List;

public class ImplOne implements Intf {
  public int pubImplOneInt;
  public String pubImplOneString;
  public List<String> pubImplOneStringList;

  // Interface Methods
  public void interfaceMethod() {}
  public void interfaceMethodWithArg(int x) {}
  public void interfaceMethodWithReturn() {}
}
