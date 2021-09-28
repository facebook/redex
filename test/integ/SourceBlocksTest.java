/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

public class SourceBlocksTest {
  String mHello = null;

  public void foo() {
    System.out.println("Hello");
  }

  public void bar() {
    baz("World");
  }

  private void baz(String s) {
    mHello = s;
  }

  public void bazz() {
    bar();
  }

  static class Scaling {
    public void no_source_blocks() {
      hot_source_blocks_inlined(true);
    }

    public void nan_source_blocks() {
      hot_source_blocks_inlined(true);
    }

    public void zero_source_blocks() {
      hot_source_blocks_inlined(true);
    }

    public void hot_source_blocks() {
      hot_source_blocks_inlined(true);
    }

    public void hot_source_blocks_inlined(boolean b) {
      if (!b) {
        System.out.println("A");
      } else {
        System.out.println("B");
      }
    }
  }
}
