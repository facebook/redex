/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexinline.otherpackage;

import static org.fest.assertions.api.Assertions.*;

public class MethodInlineOtherPackage {
  private class Foo {
    String mStr;

    protected void noninlinable() {
      if (mStr != null) {
        noninlinable();
      }
    }
  }

  private class Bar extends Foo {}

  Bar mBar = new Bar();

  public void inlineMe() {
    mBar.noninlinable();
  }
}
