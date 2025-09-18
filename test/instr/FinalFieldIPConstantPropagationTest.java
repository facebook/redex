/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import org.junit.Test;

// Test interprocedural constant propagation through final fields.

class MainConstantTest {
  public final int x;
  MainConstantTest(int x) {
      this.x = x;
  }
}

class MainIntegerBoundsTest {
  public final int x;
  MainIntegerBoundsTest(int x) {
      this.x = x;
  }
}

class MainBitsetTest {
  public final int x;
  MainBitsetTest(int x) {
      this.x = x;
  }
}

// Test interprocedural constant propagation through final fields.
public class FinalFieldIPConstantPropagationTest {
  // CHECK: method: direct redex.FinalFieldIPConstantPropagationTest.mainConstant
  @Test
  public static void mainConstant() {
    MainConstantTest ins = new MainConstantTest(1);
    if (ins.x == 0) {
      // Unreachable
      // PRECHECK: const-string {{.*}} "Hello world"
      // POSTCHECK-NOT: const-string {{.*}} "Hello world"
      System.out.println("Hello world");
    }
  }

  // CHECK: method: direct redex.FinalFieldIPConstantPropagationTest.mainIntegerBounds
  @Test
  public static void mainIntegerBounds(int n) {
    if (n > 100) {
      MainIntegerBoundsTest ins = new MainIntegerBoundsTest(n);
      if (ins.x == 0) {
        // Unreachable
        // PRECHECK: const-string {{.*}} "Hello world"
        // POSTCHECK-NOT: const-string {{.*}} "Hello world"
        System.out.println("Hello world");
      }
    }
  }

  // CHECK: method: direct redex.FinalFieldIPConstantPropagationTest.mainBitset
  @Test
  public static void mainBitset(int n) {
    n |= 0x1;
    MainBitsetTest ins = new MainBitsetTest(n);
    if (ins.x == 0) {
      // Unreachable
      // PRECHECK: const-string {{.*}} "Hello world"
      // POSTCHECK-NOT: const-string {{.*}} "Hello world"
      System.out.println("Hello world");
    }
  }
}
