/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexinline;

import static org.fest.assertions.api.Assertions.*;
import com.facebook.redexinline.otherpackage.MethodInlineOtherPackage;
import android.util.Log;
import android.os.Build;
import android.annotation.TargetApi;

import org.junit.Test;

public class MethodInlineTest {
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
      // We need this return here because we're testing what happens to code
      // after the first return statement
      return;
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

  private void hasNoninlinableInvokeDirect() {
    if (mHello != null) {
      noninlinable();
    }
  }

  @Test
  public void testInlineInvokeDirect() {
    hasNoninlinableInvokeDirect();
  }

  private static class OtherClass {
    MethodInlineTest outer;
    OtherClass(MethodInlineTest outer) { this.outer = outer; }
    private void noninlinable() {
      if (outer.mHello != null) {
        noninlinable();
      }
    }

    /*
     * This method exists to test that we rename the instance method
     * noninlinable() above when we change it to a static method. (Otherwise
     * we would have a clash in method signatures.)
     */
    public static void noninlinable(OtherClass foo) {
      if (foo != null && foo.outer != null && foo.outer.getHello() != null) {
        noninlinable(foo);
      }
    }

    public void hasNoninlinableInvokeDirect() {
      if (outer.mHello != null) {
        noninlinable();
      }
    }
  }

  /*
   * The @Test annotation here is just to ensure that the compiler doesn't
   * think the static noninlinable() method is unused and optimize it out.
   */
  @Test
  public void callsStaticNoninlinableAcrossClasses() {
    OtherClass oc = new OtherClass(this);
    oc.noninlinable(null);
  }

  @Test
  public void testInlineInvokeDirectAcrossClasses() {
    OtherClass oc = new OtherClass(this);
    oc.hasNoninlinableInvokeDirect();
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

  @Test
  public void testCalleeRefsPrivateClass() {
    (new MethodInlineOtherPackage()).inlineMe();
  }

  private int[] calleeWithFillArray() {
    int[] a = {2, 3, 4};
    return a;
  }

  @Test
  public void testFillArrayOpcode() {
    int[] a = {4, 5, 6};
    int[] b = calleeWithFillArray();
    assertThat(a[0]).isEqualTo(b[2]);
  }

  @Test
  public void testUpdateCodeSizeWhenInlining() {
    // Should not inline smallMethodThatBecomesBig.
    if (mHello == null) {
      smallMethodThatBecomesBig();
    } else {
      smallMethodThatBecomesBig();
    }
  }

  // If bigMethod gets inlined, we should not inline this method to any callers.
  private void smallMethodThatBecomesBig() {
    bigMethod();
  }

  private int bigMethod() {
    Log.e("Hello0", "World0");
    Log.e("Hello1", "World1");
    Log.e("Hello2", "World2");
    Log.e("Hello3", "World3");
    Log.e("Hello4", "World4");
    Log.e("Hello5", "World5");
    Log.e("Hello6", "World6");
    Log.e("Hello7", "World7");
    Log.e("Hello8", "World8");
    Log.e("Hello9", "World9");
    return 100;
  }

  @Test
  public void callEmpty() {
    try {
      tryStuff(0);
    } finally {
      cleanup(1);
    }
  }

  public void tryStuff(int i) {}
  public void cleanup(int i) {}

  @Test
  public void callSpecificApi() {
    // Should be inlined
    int i = NeedsAndroidJB.shouldInlineMinSdk();
    assertThat(i).isEqualTo(1);

    // Should not be inlined
    i = NeedsAndroidN.useApi();
    assertThat(i).isEqualTo(1);

    // Should not be inlined
    i = NeedsAndroidO.shouldNotInlineOutOfClass();
    assertThat(i).isEqualTo(0);

    // Should not be inlined
    i = NeedsAndroidO.shouldInlineNintoO();
    assertThat(i).isEqualTo(1);

    // Should not be inlined
    i = NeedsAndroidN.shouldNotInlineOintoN();
    assertThat(i).isEqualTo(0);

    // Should be inlined
    i = NeedsAndroidN.doesntActuallyNeedN();
    assertThat(i).isEqualTo(0);
  }

  @TargetApi(16)  // The minSdkVersion in the manifest
  private static class NeedsAndroidJB {
    @androidx.annotation.RequiresApi(api = 16)
    public static int shouldInlineMinSdk() {
      return 1;
    }
  }

  private static class NeedsAndroidN {
    @androidx.annotation.RequiresApi(api = Build.VERSION_CODES.N)
    public static int useApi() {
      return 1;
    }

    @android.support.annotation.RequiresApi(Build.VERSION_CODES.N)
    public static int shouldNotInlineOintoN() {
      return NeedsAndroidO.useApiO();
    }

    public static int doesntActuallyNeedN() {
      return 0;
    }
  }

  @TargetApi(Build.VERSION_CODES.O)
  private static class NeedsAndroidO {
    private static int shouldInlineWithinClass() {
      return 0;
    }

    public static int shouldNotInlineOutOfClass() {
      return shouldInlineWithinClass();
    }

    public static int shouldInlineNintoO() {
      // Should be inlined because N is a lower requirement than o
      return NeedsAndroidN.useApi();
    }

    public static int useApiO() {
      return 0;
    }
  }
}
