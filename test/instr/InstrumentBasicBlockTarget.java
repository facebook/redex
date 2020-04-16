/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.io.*;
import java.util.*;

// A Class used to send a message
class Sender {
  public void send(String msg) {
    System.out.println("Sending\t" + msg);
    try {
      Thread.sleep(1000);
    } catch (Exception e) {
      System.out.println("Thread  interrupted.");
    }
    System.out.println("\n" + msg + "Sent");
  }
}

// Class for send a message using Threads
class ThreadedSend extends Thread {
  private String msg;
  private Thread t;
  Sender sender;

  // Recieves a message object and a string
  // message to be sent
  ThreadedSend(String m, Sender obj) {
    msg = m;
    sender = obj;
  }

  public void run() {
    // Only one thread can send a message
    // at a time.
    synchronized (sender) {
      // synchronizing the snd object
      sender.send(msg);
    }
  }
}

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

  // This is added to test cases with more than 16 basic blocks.
  public static void testFunc4(int test_var) {
    String test_char = getCharForNumber(test_var);
    switch (test_char) {
      case "A":
        System.out.println("Random Char: A");
        break;
      case "B":
        System.out.println("Random Character: B");
        break;
      case "C":
        System.out.println("Random Character: C");
        break;
      case "D":
        System.out.println("Random Character: D");
        break;
      case "E":
        System.out.println("Random Character: BE");
        break;
      case "F":
        System.out.println("Random Character: BF");
        break;
      case "G":
        System.out.println("Random Character: BG");
        break;
      case "H":
        System.out.println("Random Character: BH");
        break;
      case "I":
        System.out.println("Random Character: BI");
        break;
      case "J":
        System.out.println("Random Character: BJ");
        break;
      case "K":
        System.out.println("Random Character: BK");
        break;
      case "L":
        System.out.println("Random Character: BL");
        break;
      case "M":
        System.out.println("Random Character: BM");
        break;
      case "N":
        System.out.println("Random Character: BN");
        break;
      case "O":
        System.out.println("Random Character: BO");
        break;
      case "P":
        System.out.println("Random Character: BP");
        break;
      case "Q":
        System.out.println("Random Character: BQ");
        break;
      case "R":
        System.out.println("Random Character: BR");
        break;
      case "S":
        System.out.println("Random Character: BS");
        break;
      case "T":
        System.out.println("Random Character: BT");
        break;
      case "U":
        System.out.println("Random Character: BU");
        break;
      case "V":
        System.out.println("Random Character: BV");
        break;
      case "W":
        System.out.println("Random Character: W");
        break;
      case "X":
        System.out.println("Random Character: X");
        break;
      case "Y":
        System.out.println("Random Character: Y");
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

  public static Long getSum(Long dateVar, int int_var) {
    return dateVar + int_var;
  }

  public static void synchronizationTest() {
    Sender snd = new Sender();
    ThreadedSend S1 = new ThreadedSend(" Hi ", snd);
    ThreadedSend S2 = new ThreadedSend(" Bye ", snd);

    // Start two threads of ThreadedSend type
    S1.start();
    S2.start();

    // Wait for both threads to finish.
    try {
      S1.join();
      S2.join();
    } catch (Exception e) {
      System.out.println("Interrupted");
    }
  }

  public static void printArray(int[] intArray) {
    for (int i = 0; i < intArray.length; i++) {
      System.out.println(intArray[i]);
    }
  }

  public static void main(String args[]) {
    boolean target = InstrumentBasicBlockTarget.testFunc2();
    InstrumentBasicBlockTarget.testFunc1();
    InstrumentBasicBlockTarget.testFunc3(target);
    Random rand = new Random();
    int temp_var = rand.nextInt(25) + 1;
    InstrumentBasicBlockTarget.testFunc4(temp_var);
    InstrumentBasicBlockTarget.testFunc6(temp_var);

    // This test is added because we were getting instrumentation
    // errors in synchronization routines in fb4a.
    InstrumentBasicBlockTarget.synchronizationTest();
    // This is added to test array usage.
    int[] intArray = new int[3];
    intArray[0] = 7001;
    intArray[1] = 3823;
    intArray[2] = 194;
    InstrumentBasicBlockTarget.printArray(intArray);
  }
}
