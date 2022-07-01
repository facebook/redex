/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
