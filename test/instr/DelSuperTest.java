/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import static org.assertj.core.api.Assertions.*;

import org.junit.Before;
import org.junit.Test;

public class DelSuperTest {
  private C1 c1;
  private C2 c2;
  private C3 c3;

  @Before
  public void setup() {
    c1 = new C1();
    c2 = new C2();
    c3 = new C3();
  }

  @Test
  public void testOptimized1() {
    // Should be optimized and C2/C3 should yield C1's response
    assertThat(c1.optimized1()).isEqualTo(-1);
    assertThat(c2.optimized1()).isEqualTo(-1);
    assertThat(c3.optimized1()).isEqualTo(-1);
  }

  @Test
  public void testOptimized2() {
    // Should be optimized and have visibility fixed after optimization
    assertThat(c1.optimized2()).isEqualTo(-2);
    assertThat(c2.optimized2()).isEqualTo(-2);
  }

  @Test
  public void testNotOptimized1() {
    // Not optimized because super is not even invoked
    assertThat(c1.notOptimized1()).isEqualTo(2);
    assertThat(c2.notOptimized1()).isEqualTo(3);
  }

  @Test
  public void testNotOptimized2() {
    // Not optimized because super is not returned
    assertThat(c1.notOptimized2()).isEqualTo(4);
    assertThat(c2.notOptimized2()).isEqualTo(5);
  }

  @Test
  public void testNotOptimized3() {
    // Not optimized because super is called with different args
    assertThat(c1.notOptimized3(100)).isEqualTo(100);
    assertThat(c2.notOptimized3(100)).isEqualTo(0);
  }

  @Test
  public void testNotOptimized4() {
    // Not optimized because super is called with args in different order
    assertThat(c1.notOptimized4(100, 101)).isEqualTo(-1);
    assertThat(c2.notOptimized4(100, 101)).isEqualTo(1);
  }

  public static class C1 {
    public int optimized1() {
      return -1;
    }
    private int optimized2() {
      return -2;
    }
    public int notOptimized1() {
      return 2;
    }
    public int notOptimized2() {
      return 4;
    }
    public int notOptimized3(int x) {
      return x;
    }
    public int notOptimized4(int x, int y) {
      return x-y;
    }
  }

  public static class C2 extends C1 {
    public int optimized1() {
      return super.optimized1();
    }
    public int optimized2() {
      return super.optimized2();
    }
    public int notOptimized1() {
      return 3;
    }
    public int notOptimized2() {
      super.notOptimized2();
      return 5;
    }
    public int notOptimized3(int x) {
      return super.notOptimized3(0);
    }
    public int notOptimized4(int x, int y) {
      return super.notOptimized4(y, x);
    }
  }

  public static class C3 extends C2 {
    public int optimized1() {
      return super.optimized1();
    }
  }
}
