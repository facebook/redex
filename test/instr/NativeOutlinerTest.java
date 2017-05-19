/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.instr;

import org.junit.Test;

public class NativeOutlinerTest {

  @Test
  public void test() {
  }

  public void outlinedError() {
    throw new Error("Outlined Error __TEST__");
  }

  public void outlinedRuntimeException() {
    throw new RuntimeException("Outlined RuntimeException __TEST__");
  }

  public void notOutlined() {
    throw new RuntimeException("Not Outlined" + System.currentTimeMillis());
  }
}
