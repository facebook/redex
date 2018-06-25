/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;

public class UnterfaceTest {
  private String mHello = "hello";
  private String mWorld = "world";

  /**
   * TODO: Document this test!!!
   */
  @Test
  public void test() {
    GetText hello = new GetText() {
      public String getText() {
        return mHello + " ";
      }
    };

    GetText world = new GetText() {
      public String getText() {
        return mWorld + "!";
      }
    };

    assertThat(hello.getText() + world.getText()).isEqualTo("hello world!");
  }
}

interface GetText {
  String getText();
}
