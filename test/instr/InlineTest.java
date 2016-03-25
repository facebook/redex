/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redexinline;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;

public class InlineTest {
  String mHello = null;

  /**
   * TODO: Document this test!!!
   */
  @Test
  public void test() {
    setHello("Hello");
    assertThat(getHello() + addWorld()).isEqualTo("Hello World");
    assertThat(likely(true)).isTrue();
  }

  private String getHello() {
    return mHello;
  }

  private void setHello(String hello) {
    mHello = hello;
  }

  private static String addWorld() {
    return " World";
  }

  public static boolean likely(boolean b) {
    return b;
  }
}
