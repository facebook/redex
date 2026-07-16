/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

  private static String mWorld;

  private static void bazzz() {
    mWorld = "WORLD";
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

    public void set_world() {
      // Generate a field accessor.
      SourceBlocksTest.mWorld = "World";
    }

    public void call_bazzz() {
      // Generate a method accessor.
      SourceBlocksTest.bazzz();
    }
  }

  static class ViolationsFixTest {

    public void idom_branch() {
      int x = 0;
      if (x == 1) {
        branch_1();
      } else {
        branch_2();
      }
    };

    public void branch_1() {
      System.out.println("x = 0");
    }

    public void branch_2() {
      System.out.println("x = 1");
    }

     public void hot_method() {
      System.out.print("Cold Entry");
    }

    public void cold_method() {
      System.out.print("Cold Entry");
    }

    public void hot_method_2() {
      System.out.print("Hot Entry");
    }

  }

  static class ChainAndDomClass {

    public void chains() {
      int i = 0;
      System.out.println(i);
    }

    public void throwable() throws RuntimeException {
      throw new RuntimeException();
    }

    public void thrower(int j) {
      j = 1;
      throwable();
      j++;
    }
  }

  static class IntermethodViolationsClass {

    public IntermethodViolationsClass() {
      boolean run_callee = true;
      if (run_callee) {
        callee();
      } else {
        return;
      }
     }

    public int callee() {
      int i = 0;
      return i;
    }
  }

    static class IDomBlockCounting {

    public void idom(int x) {
      System.out.println("start");
      if (x == 1) {
        System.out.println("x = 1");
      } else {
        System.out.println("x = ?");
      }
    };
  }
}
