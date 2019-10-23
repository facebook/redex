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
}
