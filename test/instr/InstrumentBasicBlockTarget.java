/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.io.*;
import java.util.*;

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
  public static int testFunc4(int a, int b) {
    Random rand = new Random();
    try {
      int[] arr = new int[10];
      arr[10] = a;
    } catch (Exception e) {
      throw e;
    }
    return (rand.nextInt(a) % 2);
  }

  @DoNotStrip
  public static void testFunc5(int flag) {
    Random rand = new Random();
    int temp_var = rand.nextInt(10);
    if (flag != 0) {
      System.out.println("It's True!!");
      if (temp_var > 4) {
        System.out.println("Greater than 4");
      } else {
        System.out.println("Couldnt make it to 4!, early return");
        return;
      }
    } else {
      System.out.println("Not True :(");
    }
    System.out.println("After Test: " + temp_var);
    InstrumentBasicBlockTarget.testFunc1(2);
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

  @DoNotStrip
  public static void main(String args[]) {
    Random rand = new Random();
    System.out.println(testFunc9(rand.nextInt(100)));
  }
}
