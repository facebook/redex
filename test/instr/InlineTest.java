/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redexinline;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Test;

public class InlineTest {
  String mHello = null;

  /**
   * TODO: Document this test!!!
   */
  @Test
  public void test() {
    setHello("Hello");
    assertThat(getHello() + addWorld()).isEqualTo("Hello World");
    assertThat(likely(true)).isTrue();
  }

  private String getHello() {
    return mHello;
  }

  private void setHello(String hello) {
    mHello = hello;
  }

  private static String addWorld() {
    return " World";
  }

  public static boolean likely(boolean b) {
    return b;
  }

  private void needsInvokeRange(int a, String b, char c, long d, double e) {
    assertThat(a).isEqualTo(0);
    assertThat(b).isEqualTo("a");
    assertThat(c).isEqualTo('c');
    assertThat(d).isEqualTo(123);
    assertThat(e).isEqualTo(1.1);
  }

  @Test
  public void testInvokeRange() {
    needsInvokeRange(0, "a", 'c', 123, 1.1);
  }

  public void wrapsThrow() throws Exception {
    throw new Exception("foo");
  }

  private void throwsInElse() throws Exception {
    if (mHello != null) {
      String y = "x";
    } else {
      wrapsThrow();
    }
  }

  @Test
  public void testCallerTryCalleeElseThrows() {
    boolean caught = false;
    try {
      throwsInElse();
    } catch (Exception e) {
      caught = true;
    }
    assertThat(caught).isTrue();
  }

  private void throwsInIf() throws Exception {
    if (mHello == null) {
      wrapsThrow();
    } else {
      String y = "x";
    }
  }

  @Test
  public void testCallerTryCalleeIfThrows() {
    boolean caught = false;
    try {
      throwsInIf();
    } catch (Exception e) {
      caught = true;
    }
    assertThat(caught).isTrue();
  }

  private void throwsInElse2() throws Exception {
    if (mHello != null) {
      String y = "x";
    } else {
      wrapsThrow();
    }
  }

  @Test
  public void testCallerNestedTry() {
    boolean caught = false;
    try {
      try {
        throw new Exception("bar");
      } catch (Exception e) {
        throwsInElse2();
      }
    } catch (Exception e) {
      caught = true;
    }
    assertThat(caught).isTrue();
  }

  private void throwsUncaught() throws Exception {
    try {
      wrapsThrow();
    } catch (ArithmeticException e) {
    }
  }

  @Test
  public void testCalleeTryUncaught() {
    boolean caught = false;
    try {
      throwsUncaught();
    } catch (Exception e) {
      caught = true;
    }
    assertThat(caught).isTrue();
  }

  public void wrapsArithmeticThrow() throws Exception {
    throw new ArithmeticException("foo");
  }

  private void throwsCaught() throws Exception {
    try {
      wrapsArithmeticThrow();
    } catch (ArithmeticException e) {
    }
  }

  @Test
  public void testCalleeTryCaught() {
    boolean caught = false;
    try {
      throwsCaught();
    } catch (Exception e) {
      caught = true;
    }
    assertThat(caught).isFalse();
  }

  private void handlerThrows() throws Exception {
    try {
      wrapsArithmeticThrow();
    } catch (ArithmeticException e) {
      wrapsThrow();
    }
  }

  @Test
  public void testCalleeTryHandlerThrows() {
    boolean caught = false;
    try {
      handlerThrows();
    } catch (Exception e) {
      caught = true;
    }
    assertThat(caught).isTrue();
  }

  private void inlineCalleeTryOnce() throws Exception {
    try {
      wrapsThrow();
    } catch (ArrayIndexOutOfBoundsException e) {
      System.out.println("");
    }
  }

  private void inlineCalleeTryTwice() throws Exception {
    try {
      inlineCalleeTryOnce();
    } catch (ArithmeticException e) {
      System.out.println("");
    }
  }

  @Test
  public void testInlineCalleeTryTwice() {
    boolean caught = false;
    try {
      inlineCalleeTryTwice();
    } catch (Exception e) {
      caught = true;
    }
    assertThat(caught).isTrue();
  }

  private void noninlinable() {
    if (mHello != null) {
      noninlinable();
    }
  }

  /*
   * This method exists to test that we rename the instance method
   * noninlinable() above when we change it to a static method. (Otherwise
   * we would have a clash in method signatures.)
   */
  public static void noninlinable(InlineTest foo) {
    if (foo != null && foo.getHello() != null) {
      noninlinable(foo);
    }
  }

  /*
   * The @Test annotation here is just to ensure that the compiler doesn't
   * think the static noninlinable() method is unused and optimize it out.
   */
  @Test
  public void callsStaticNoninlinable() {
    noninlinable(null);
  }

  private void hasNoninlinableInvokeDirect() {
    if (mHello != null) {
      noninlinable();
    }
  }

  @Test
  public void testInlineInvokeDirect() {
    hasNoninlinableInvokeDirect();
  }

  private void throwsWithNoReturn() throws Exception {
    if (mHello != null) {
      throw new Exception("foo");
    } else {
      throw new ArithmeticException("foo");
    }
  }

  @Test
  public void testThrowsWithNoReturn() throws Exception {
    boolean caught = false;
    try {
      throwsWithNoReturn();
    } catch (ArithmeticException e) {
      caught = true;
    }
    assertThat(caught).isTrue();
  }

  @Test
  public void testArrayDataInCaller() throws Exception {
    int[] arr = {3, 1, 2};
    calleeWithIf();
    assertThat(likely(true)).isTrue();
    assertThat(arr[0]).isEqualTo(3);
  }

  private void calleeWithIf() throws Exception {
    if (mHello != null) {
      wrapsThrow();
      return;
    }
    String y = "x";
  }

  @ForceInline
  private String multipleCallers() {
    return Integer.toString(1) + Integer.toString(2);
  }

  @Test
  public void testForceInlineOne() {
    multipleCallers();
  }

  @Test
  public void testForceInlineTwo() {
    multipleCallers();
  }
}
