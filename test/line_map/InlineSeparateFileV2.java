/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
