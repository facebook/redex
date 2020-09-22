/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

import static org.fest.assertions.api.Assertions.*;

import android.test.InstrumentationTestCase;
import java.io.*;
import java.util.ArrayList;
import java.util.Arrays;
import org.junit.*;

public class InlineMainV2 extends InstrumentationTestCase {

  LineMapperV2 lm;

  @Before
  public void setUp() throws Exception {
    super.setUp();
    lm = new LineMapperV2(
      getInstrumentation().getTargetContext().getResources().getAssets().open(
        "redex-line-number-map-v2"));
  }

  /**
   * Check that the stack trace of a non-inlined function makes sense.
   */
  @Test
  public void testBasic() throws Exception {
    try {
      InlineSeparateFileV2.wrapsThrow();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(
          Arrays.asList(
              "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
              "com.facebook.redexlinemap.InlineMainV2.testBasic(InlineMainV2.java:36)"),
          TraceUtil.traceToString(trace, 2));
    }
  }

  /**
   * Check the stack trace of a once-inlined function.
   */
  @Test
  public void testInlineOnce() throws Exception {
    try {
      InlineSeparateFileV2.inlineOnce();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(
          Arrays.asList(
              "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
              "com.facebook.redexlinemap.InlineSeparateFileV2.inlineOnce(InlineSeparateFileV2.java:16)",
              "com.facebook.redexlinemap.InlineMainV2.testInlineOnce(InlineMainV2.java:53)"),
          TraceUtil.traceToString(trace, 3));
    }
  }

  /**
   * Check the stack trace of a multiply-inlined function.
   */
  @Test
  public void testInlineTwice() throws Exception {
    try {
      InlineSeparateFileV2.inlineTwice();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(
          Arrays.asList(
              "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
              "com.facebook.redexlinemap.InlineSeparateFileV2.inlineOnce1(InlineSeparateFileV2.java:20)",
              "com.facebook.redexlinemap.InlineSeparateFileV2.inlineTwice(InlineSeparateFileV2.java:24)",
              "com.facebook.redexlinemap.InlineMainV2.testInlineTwice(InlineMainV2.java:71)"),
          TraceUtil.traceToString(trace, 4));
    }
  }

  private static int inlineMe() {
    return 1;
  }

  @NoInline
  private void ignoreAndThrow(Object x) throws Exception {
    throw new Exception("foo");
  }

  /**
   * Check that the line number is reset to the original for the code that
   * follows an inlined method. We expect the resulting stack trace *not*
   * to point to inlineMe().
   */
  @Test
  public void testPositionReset() throws Exception {
    try {
      ignoreAndThrow(inlineMe());
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(
          Arrays.asList(
              "com.facebook.redexlinemap.InlineMainV2.ignoreAndThrow(InlineMainV2.java:90)",
              "com.facebook.redexlinemap.InlineMainV2.testPositionReset(InlineMainV2.java:101)"),
          TraceUtil.traceToString(trace, 2));
    }
  }

  private void elseThrows() throws Exception {
    if (lm == null) {
      System.out.println("");
    } else {
      InlineSeparateFileV2.wrapsThrow();
    }
  }

  @Test
  public void testElseThrows() throws Exception {
    try {
      elseThrows();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(
          Arrays.asList(
              "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
              "com.facebook.redexlinemap.InlineMainV2.elseThrows(InlineMainV2.java:116)",
              "com.facebook.redexlinemap.InlineMainV2.testElseThrows(InlineMainV2.java:123)"),
          TraceUtil.traceToString(trace, 3));
    }
  }
}
