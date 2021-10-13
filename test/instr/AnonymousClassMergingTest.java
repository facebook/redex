/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * This is a seemingly simple test for AnonymousClassMerging.
 * However, the pass will pick up other classes bundled in the testing apk, e.g., 'Lcom/google/..'.
 * We had to apply some filtering to narrow down the scope of the merging transformation.
 * The test covers more obscure default interface method cases such as an external default method implementation.
 */

package com.facebook.redextest;

import org.junit.Test;

import static org.fest.assertions.api.Assertions.assertThat;

interface Interface1 {
  default int magic1() { return 42; }
  int magic2();
}

public class AnonymousClassMergingTest {

  @Test
  public void testDefaultInterfaceMethod() {
    Interface1 a = new Interface1() {
      @Override
      public int magic2() {
        return 142;
      }
    };
    assertThat(a.magic1()).isEqualTo(42);
    assertThat(a.magic2()).isEqualTo(142);

    Interface1 b = new Interface1() {
      @Override
      public int magic2() {
        return 143;
      }
    };
    assertThat(b.magic1()).isEqualTo(42);
    assertThat(b.magic2()).isEqualTo(143);

    Interface1 c = new Interface1() {
      @Override
      public int magic2() {
        return 144;
      }
    };
    assertThat(c.magic1()).isEqualTo(42);
    assertThat(c.magic2()).isEqualTo(144);

    Interface1 d = new Interface1() {
      @Override
      public int magic2() {
        return 145;
      }
    };
    assertThat(d.magic1()).isEqualTo(42);
    assertThat(d.magic2()).isEqualTo(145);

    Interface1 e = new Interface1() {
      @Override
      public int magic2() {
        return 146;
      }
    };
    assertThat(e.magic1()).isEqualTo(42);
    assertThat(e.magic2()).isEqualTo(146);
  }
}
