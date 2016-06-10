/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redexlinemap;

public class InlineSeparateFile {
  public static final void wrapsThrow() throws Exception {
    throw new Exception("foo");
  }
  public static final void inlineMe() throws Exception {
    String foo = null;
    wrapsThrow();
  }
  public static final void inlineMe2() throws Exception {
    String foo = null;
    wrapsThrow();
  }
  public static final void inlineNested() throws Exception {
    String foo = null;
    inlineMe2();
  }
}
