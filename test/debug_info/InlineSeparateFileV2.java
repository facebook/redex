/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

public class InlineSeparateFileV2 {
  public static final void wrapsThrow() throws Exception {
    throw new Exception("foo");
  }
  public static final void inlineOnce() throws Exception {
    String foo = null;
    wrapsThrow();
  }
  public static final void inlineOnce1() throws Exception {
    String foo = null;
    wrapsThrow();
  }
  public static final void inlineTwice() throws Exception {
    String foo = null;
    inlineOnce1();
  }

  public static int counter;

  @NoInline
  public static void wrapsThrow2() throws Exception {
    if (--counter == 0) {
      throw new Exception("foo2");
    }
  }

  @NoInline
  public static void outlinedThrower() throws Exception {
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
    wrapsThrow2();
  }

  @NoInline
  public static void wrapsThrow3() throws Exception {
    if (--counter == 0) {
      throw new Exception("foo3");
    }
  }

  @ForceInline
  public static void wrapsThrowInline() throws Exception {
    wrapsThrow3();
  }

  @NoInline
  public static void outlinedThrowerInlined() throws Exception {
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
    wrapsThrowInline();
  }
}
