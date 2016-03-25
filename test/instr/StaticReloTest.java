/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.instr;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;

/**
 * This test is not capable of testing the following aspects of StaticRelo:
 *
 * - PGO logic, e.g. choosing the best recipient target
 * - Dex logic, e.g. methods move to a class in the same dex
 *
 * KeepForRedexTest are to ensure that the inliner doesn't inline these methods
 *
 */
public class StaticReloTest {

  private static final String PKG = "com.facebook.redex.test.instr";
  private static Class getClass(String pkg, String cls) {
    try {
      return Class.forName(pkg+"."+cls);
    } catch (ClassNotFoundException e) {
      return null;//throw new RuntimeException(e);
    }
  }

  @Test
  public void testFunctional() {
    assertThat(C1.m1()).isEqualTo(1);
    assertThat(C1.dupe()).isEqualTo(100);
    assertThat(C2.m2()).isEqualTo(2);
    assertThat(C2.dupe()).isEqualTo(101);
    assertThat(C3.m3()).isEqualTo(3);
    assertThat(C4.m4()).isEqualTo(4);
    assertThat(C5.m5()).isEqualTo(5);
    assertThat(new C6().v()).isEqualTo(0);
    assertThat(C6.m6()).isEqualTo(6);
    assertThat(C7.m7()).isEqualTo(7);
    // Just here to force construction of a C7
    assertThat(new C7()).isNotNull();
  }

  /**
   * N.B. This structural test is pretty weak. Almost always both C1 and C2
   * should be deleted, but sometimes, if StaticReloTest is the dex
   * relocation target, only one will be relocatable. So the test should
   * look more like "either C1 and C2 are deleted or StaticReloTest is the
   * relo target, C1#m1 and C2#m2 moved there, and either C1#dupe or
   * C2#dupe moved there as well."
   */
  @Test
  public void testStructural() {
    // Ensure that either C1 or C2 (or both) have been deleted!
    assertThat(
      getClass(PKG, "StaticReloTest$C1") == null ||
      getClass(PKG, "StaticReloTest$C2") == null).isTrue();
    // Ensure other classes are in place
    assertThat(getClass(PKG, "StaticReloTest$C3")).isNotNull();
    assertThat(getClass(PKG, "StaticReloTest$C4")).isNotNull();
    assertThat(getClass(PKG, "StaticReloTest$C5")).isNotNull();
    assertThat(getClass(PKG, "StaticReloTest$C6")).isNotNull();
    assertThat(getClass(PKG, "StaticReloTest$C7")).isNotNull();
  }

  public final static class C1 {
    // Moveable
    public static int m1() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return 1;
    }

    // Either this or C2#dupe should move to StaticReloTest
    public static int dupe() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return 100;
    }
  }

  public final static class C2 {
    // Moveable
    public static int m2() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return 2;
    }

    // Either this or C1#dupe should move to StaticReloTest
    public static int dupe() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return 101;
    }
  }

  // Not moveable due to static fields
  public final static class C3 {
    static int i = 3;
    public static int m3() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return i;
    }
  }

  // Not moveable due to instance fields fields
  public final static class C4 {
    int i = 4;
    public static int m4() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return 4;
    }
  }

  // Not moveable due to static constructor
  public final static class C5 {
    static {
      System.out.println("foo");
    }

    public static int m5() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return 5;
    }
  }

  // Not moveable due to virtual method
  public final static class C6 {
    @KeepForRedexTest
    public int v() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return 0;
    }

    public static int m6() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return 6;
    }
  }

  // Not moveable due to being constructed
  public final static class C7 {
    public static int m7() {
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      System.out.println("don't inline me");
      return 7;
    }
  }
}
