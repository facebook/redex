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

import com.facebook.redextest.InstrumentBasicBlockAnalysis;

@DoNotStrip
public class InstrumentBasicBlockTarget {
  // Information taken from source dictionary and metadata
  // Index: 0
  // Offset within Stats Array: 8
  @DoNotStrip
  public static int testFunc01(int foo) {
    return 42;
  }

  // Index: 1
  // Offset within Stats Array: 10
  // Bit-Vector 0: [2,1]
  @DoNotStrip
  public static int testFunc02(int a, int b) {
    if (a > 0) {
      return a / b;
    } else {
      return a * b;
    }
  }

  // Index: 2
  // Offset within Stats Array: 13
  // Bit-Vector 0: [2,1]
  @DoNotStrip
  public static int testFunc03(int a, int b) {
    if (a > 0) {
      a++;
    }
    return a;
  }

  // Index: 3
  // Offset within Stats Array: 16
  // Bit-Vector 0: [5,4,2,1]
  @DoNotStrip
  @SuppressWarnings({"EmptyCatchBlock", "CatchGeneralException"})
  public static int testFunc04(int a, int b) {
    try {
      int[] arr = new int[10];
      arr[10] = a;
    } catch (Exception e) {
       throw e;
    }
    return (b % 2);
  }

  // Index: 4
  // Offset within Stats Array: 19
  // Bit-Vector 0: [4,3,5,2,1]
  @DoNotStrip
  public static int testFunc05(int flag, int temp_var) {
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

  // Index: 5
  // Offset within Stats Array: 22
  // Bit-Vector 0: [4,3,2,1]
  @DoNotStrip
  public static int testFunc06(int a, int b) {
    try {
      return a / b;
    } catch (ArithmeticException e) {
      System.out.println("ArithmeticException");
      return 13;
    }
  }

  // Index: 6
  // Offset within Stats Array: 25
  // Bit-Vector 0: [9,8,7,6,4,3,2,1]
  @DoNotStrip
  public static int testFunc07(int a, int b) {
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

  // Index: 7
  // Offset within Stats Array: 28
  // Bit-Vector 0: [9,10,12,11,8,7,6,5,4,3,2,1]
  @DoNotStrip
  @SuppressWarnings("CatchGeneralException")
  public static int testFunc08(int size, int index, int num) {
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

  // Index: 8
  // Offset within Stats Array: 31
  // Bit-Vector 0: [43,44,45,46,47,48,49,50,51,52,53,54,55,3,2,1]
  // Bit-Vector 1: [27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42]
  // Bit-Vector 2: [11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26]
  // Bit-Vector 3: [4,5,6,7,8,9,10]
  @DoNotStrip
  public static String testFunc09(int test_var) {
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

  // Index: 9
  // Offset within Stats Array: 37
  // Bit-Vector 0: [0,1,3,4]
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
  public void test01() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    assertThat(testFunc01(0)).isEqualTo(42);
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[9]).isEqualTo((short)1);

    // TestFunc01 has no bitvector because it only has one basicblock
    // that always executes so we do not need to assert it
  }

  @Test
  @DoNotStrip
  public void test02() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    assertThat(testFunc02(21,7)).isEqualTo(3);
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[11]).isEqualTo((short)1);

    // Assert that TestFunc02 excuted only BasicBlocks (1) skipping
    // 2 which is [0,1] in the Bit-Vector form due to return in the if condition
    assertThat(stats[12]).isEqualTo((short)0b01);
  }

  @Test
  @DoNotStrip
  public void test03() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    assertThat(testFunc03(8,9)).isEqualTo(9);
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[14]).isEqualTo((short)1);

    // Assert that TestFunc03 excuted all BasicBlocks
    // which is [1,1] in the Bit-Vector form
    assertThat(stats[15]).isEqualTo((short)0b11);
  }

  @Test
  @DoNotStrip
  @SuppressWarnings("CatchGeneralException")
  public void test04() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    boolean thrown = false;
    try {
      testFunc04(10,16);
    } catch (Exception e) {
      System.out.println("Exeception Thrown");
      thrown = true;
    }
    assertThat(thrown).isTrue();
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[17]).isEqualTo((short)1);

    // Assert that TestFunc04 excuted only BasicBlocks (1,2,5) skipping
    // 3 which is [1,0,1,1] in the Bit-Vector form due to ArrayOutOfBounds exception
    assertThat(stats[18]).isEqualTo((short)0b1011);
  }

  @Test
  @DoNotStrip
  public void test05() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    assertThat(testFunc05(0,1)).isEqualTo(-1);
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[20]).isEqualTo((short)1);

    // Assert that TestFunc05 excuted only BasicBlocks (4,5) skipping
    // 1,2,3 which is [1,0,1,0,0] in the Bit-Vector form due to flag being zero
    assertThat(stats[21]).isEqualTo((short)0b10100);
  }

  @Test
  @DoNotStrip
  public void test06() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    assertThat(testFunc06(9,1)).isEqualTo(9);
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[23]).isEqualTo((short)1);

    // Assert that TestFunc05 excuted only BasicBlocks (1,2,3) skipping
    // 4 which is [0,1,1,1] in the Bit-Vector form due to early return in exception
    assertThat(stats[24]).isEqualTo((short)0b0111);
  }

  @Test
  @DoNotStrip
  public void test07() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    assertThat(testFunc07(8,2)).isEqualTo(4);
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[26]).isEqualTo((short)1);

    // Assert that TestFunc07 excuted only BasicBlocks (1,2,3) skipping
    // 4,6,7,8,9 which is [0,0,0,0,0,1,1,1] in the Bit-Vector form due 8 % 2 = 0 and
    // there was no exception handling needed so it returned early
    assertThat(stats[27]).isEqualTo((short)0b0111);
  }

  @Test
  @DoNotStrip
  @SuppressWarnings("CatchGeneralException")
  public void test08() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    int value = 0;
    try {
      value = testFunc08(5,2,8);
    } catch (Exception e) {
      System.out.println("Exeception Thrown");
    }
    assertThat(value).isEqualTo(7);
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[29]).isEqualTo((short)1);

    // Assert that TestFunc08 excuted only BasicBlocks (1,2,3,4,5,6,7,8,9) skipping
    // 10,11,12 which is [1,0,0,0,1,1,1,1,1,1] in the Bit-Vector form due to
    // index being 2 and 2 % 2 = 0 which causes an Array Index Out of Bounds Exception
    // before it returns early
    assertThat(stats[30]).isEqualTo((short)0b1000111111);
  }

  @Test
  @DoNotStrip
  public void test09() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    assertThat(testFunc09(16)).isEqualTo("purge");
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[32]).isEqualTo((short)1);

    // Assert that TestFunc09 excuted only BasicBlocks (2,40) skipping
    // everything else because it didn't have to go into the initial if condition
    // because test-flag > 0 and the switch statement made it jump to exact basicblock
    // of 16 and returning immediately. As this test case has numerous basicblocks (55),
    // it needs four bitvectors so we need to make sure it is [0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0] in
    // Bit-Vector 1 and [0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0] in Bit-Vector 2 while making sure the last
    // two Bit-Vectors are only zeroes.
    assertThat(stats[33]).isEqualTo((short)0b0000000000000010);
    assertThat(stats[34]).isEqualTo((short)0b0000000000000100);
    assertThat(stats[35]).isEqualTo((short)0b0000000000000000);
    assertThat(stats[36]).isEqualTo((short)0b0000000);
  }

  @Test
  @DoNotStrip
  public void test10() {
    // Start Tracing Information and run Function before stopping
    InstrumentBasicBlockAnalysis.startTracing();
    assertThat(testFunc10()).isFalse();
    InstrumentBasicBlockAnalysis.stopTracing();

    // Get Stats from Instrument Analysis
    short[] stats = InstrumentBasicBlockAnalysis.getStats();

    // Assert that only one function was executed at all
    // because we cleanup stats array before every tracing
    assertThat(stats[38]).isEqualTo((short)1);

    // Assert that TestFunc10 excuted only BasicBlocks (0,1,4) skipping
    // 3 which is [1,0,1,1] in the Bit-Vector form due to exception
    assertThat(stats[39]).isEqualTo((short)0b1011);
  }
}
