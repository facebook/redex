/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * <p>This source code is licensed under the MIT license found in the LICENSE file in the root
 * directory of this source tree.
 */
package com.facebook.redextest;

import java.util.*;

public class InstrumentBasicBlockTarget {
  public static void testFunc1() {
    int x = 1;
  }

  public static boolean testFunc2() {
    Random rand = new Random();
    return (rand.nextInt(10) % 2) == 0;
  }

  public static void testFunc3(boolean flag) {
    Random rand = new Random();

    int temp_var = rand.nextInt(10);
    if (flag) {
      System.out.println("It's True!!");
      if (temp_var > 4) {
        System.out.println("Above 4");
      } else {
        System.out.println("Couldnt make it to 4!");
      }
    } else {
      System.out.println("Not True :(");
    }
    System.out.println("After Test: " + temp_var);
    InstrumentBasicBlockTarget.testFunc1();
  }

  private static String getCharForNumber(int i) {
    return i > 0 && i < 27 ? String.valueOf((char) (i + 'A' - 1)) : null;
  }

  public static void testFunc4(int test_var) {
    String test_char = getCharForNumber(test_var);
    switch (test_char) {
      case "A":
        System.out.println("Random Char: A");
        break;
      case "B":
        System.out.println("Random Character: B");
        break;
      default:
        System.out.println("Charater not allowed.");
    }
  }

  public static void testFunc5() {
    System.out.println("Test Var value: 20 Found!!");
  }

  public static void testFunc6(int test_var) {
    int in = 0;
    while (in++ < test_var) {
      System.out.println("In the loop: " + in);
      if (in == 20) {
        InstrumentBasicBlockTarget.testFunc5();
      }
    }
    Long date_in_Long = getDateinLong();
    System.out.println("Sum :: " + getSum(date_in_Long, test_var));
  }

  public static Long getDateinLong() {
    return new Date().getTime();
  }

 public static Long getSum(Long dateVar, int int_var){
   return dateVar + int_var;
 }
  public static void main(String args[]) {
    boolean target = InstrumentBasicBlockTarget.testFunc2();
    InstrumentBasicBlockTarget.testFunc1();
    InstrumentBasicBlockTarget.testFunc3(target);
    Random rand = new Random();
    int temp_var = rand.nextInt(25) + 1;
    InstrumentBasicBlockTarget.testFunc4(temp_var);
    InstrumentBasicBlockTarget.testFunc6(temp_var);
  }
}
