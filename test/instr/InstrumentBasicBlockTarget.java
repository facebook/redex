/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;


import static org.fest.assertions.api.Assertions.assertThat;

import java.io.*;
import java.util.*;
import org.junit.Test;

import com.facebook.proguard.annotations.DoNotStrip;

@DoNotStrip
public class InstrumentBasicBlockTarget {
  @DoNotStrip
  public static int testFunc1(int foo) {
    return 42;
  }

  @DoNotStrip
  public static int testFunc2(int a, int b) {
    if (a > 0) {
      return a / b;
    } else {
      return a * b;
    }
  }

  @DoNotStrip
  public static int testFunc3(int a, int b) {
    if (a > 0) {
      a++;
    }
    return a;
  }

  @DoNotStrip
  @SuppressWarnings({"EmptyCatchBlock", "CatchGeneralException"})
  public static int testFunc4(int a, int b) {
    try {
      int[] arr = new int[10];
      arr[10] = a;
    } catch (Exception e) {
       throw e;
    }
    return (b % 2);
  }

  @DoNotStrip
  public static int testFunc5(int flag, int temp_var) {
    int z = 0;
    if (flag != 0) {
      System.out.println("It's True!!");
      z += 1;
      if (temp_var > 4) {
        System.out.println("Greater than 4");
        z += 1;
      } else {
        System.out.println("Couldnt make it to 4!, early return");
        z -= 1;
        return z;
      }
    } else {
      System.out.println("Not True :(");
      z -= 1;
    }
    System.out.println("After Test: " + temp_var);
    return z;
  }

  @DoNotStrip
  public static int testFunc6(int a, int b) {
    try {
      return a / b;
    } catch (ArithmeticException e) {
      System.out.println("ArithmeticException");
      return 13;
    }
  }

  @DoNotStrip
  public static int testFunc7(int a, int b) {
    if (a % 2 == 0) {
      try {
        return a / b;
      } catch (IllegalArgumentException e) {
        throw new IllegalArgumentException("Exception: " + a + ": " + e.getMessage());
      }
    } else {
      try {
        return b / a;
      } catch (IllegalArgumentException e) {
        throw new IllegalArgumentException("Exception: " + b + ": " + e.getMessage());
      }
    }
  }

  @DoNotStrip
  public static int testFunc8(int size, int index, int num) {
    try {
      int[] arr = new int[size];
      arr[index] = num / index;
      if (index % 2 == 0) {
        index = index * 2020;
        return arr[index];
      }
    } catch (ArrayIndexOutOfBoundsException e) {
      System.out.println("ArrayIndexOutOfBoundsException");
      return 7;
    } catch (ArithmeticException e) {
      System.out.println("ArithmeticException");
      return 13;
    } catch (Exception e) {
      System.out.println("Exception");
      throw e;
    }
    return 9;
  }

  // This is added to test cases with more than 16 basic blocks.
  @DoNotStrip
  public static String testFunc9(int test_var) {
    if (test_var < 0) {
      test_var = test_var * -1;
    }
    switch (test_var) {
      case 1:
        return "apple";
      case 2:
        return "banana";
      case 3:
        return "cat";
      case 4:
        return "dog";
      case 5:
        return "eat";
      case 6:
        return "fat";
      case 7:
        return "go";
      case 8:
        return "hi";
      case 9:
        return "ice";
      case 10:
        return "juice";
      case 11:
        return "kim";
      case 12:
        return "love";
      case 13:
        return "mmm";
      case 14:
        return "ninja";
      case 15:
        return "orange";
      case 16:
        return "purge";
      case 17:
        return "qwerty";
      case 18:
        return "roll";
      case 19:
        return "star";
      case 20:
        return "tar";
      case 21:
        return "ufo";
      case 22:
        return "void";
      case 23:
        return "woman";
      case 24:
        return "x";
      case 25:
        return "yellow";
      case 26:
        return "zz";
      case 27:
        return "aapple";
      case 28:
        return "bbanana";
      case 29:
        return "ccat";
      case 30:
        return "ddog";
      case 31:
        return "eeat";
      case 32:
        return "ffat";
      case 33:
        return "ggo";
      case 34:
        return "hhi";
      case 35:
        return "iice";
      case 36:
        return "jjuice";
      case 37:
        return "kkim";
      case 38:
        return "llove";
      case 39:
        return "mmmm";
      case 40:
        return "nninja";
      case 41:
        return "oorange";
      case 42:
        return "ppurge";
      case 43:
        return "qqwerty";
      case 44:
        return "rroll";
      case 45:
        return "sstar";
      case 46:
        return "ttar";
      case 47:
        return "uufo";
      case 48:
        return "vvoid";
      case 49:
        return "wwoman";
      case 50:
        return "xx";
      case 51:
        return "yyellow";
      case 52:
        return "zzz";
    }
    return "...";
  }

  @DoNotStrip
  public static boolean testFunc10() {
    try {
      Class.forName("com.facebook.Foo");
      return true;
    } catch (ClassNotFoundException e) {
      System.out.println("can't load it");
      return false;
    }
  }

  @Test
  @DoNotStrip
  public void test1() {
    assertThat(testFunc1(0)).isEqualTo(42);
  }

  @Test
  @DoNotStrip
  public void test2() {
    assertThat(testFunc2(21,7)).isEqualTo(3);
  }

  @Test
  @DoNotStrip
  public void test3() {
    assertThat(testFunc3(8,9)).isEqualTo(9);
  }

  @Test
  @DoNotStrip
  @SuppressWarnings("CatchGeneralException")
  public void test4() {
    boolean thrown = false;
    try {
      testFunc4(10,16);
    } catch (Exception e) {
      System.out.println("Exeception Thrown");
      thrown = true;
    }
    assertThat(thrown).isTrue();
  }

  @Test
  @DoNotStrip
  public void test5() {
    assertThat(testFunc5(0,1)).isEqualTo(-1);
  }

  @Test
  @DoNotStrip
  public void test6() {
    assertThat(testFunc6(9,1)).isEqualTo(9);
  }

  @Test
  @DoNotStrip
  public void test7() {
    assertThat(testFunc7(8,2)).isEqualTo(4);
  }

  @Test
  @DoNotStrip
  public void test8() {
    assertThat(testFunc8(5,2,8)).isEqualTo(7);
  }

  @Test
  @DoNotStrip
  public void test9() {
    assertThat(testFunc9(16)).isEqualTo("purge");
  }

  @Test
  @DoNotStrip
  public void test10() {
    assertThat(testFunc10()).isFalse();
  }
}
