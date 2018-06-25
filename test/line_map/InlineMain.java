/**
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

public class InlineMain extends InstrumentationTestCase {

  LineMapper lm;

  public void setUp() throws Exception {
    super.setUp();
    lm = new LineMapper(
      getInstrumentation().getTargetContext().getResources().getAssets().open(
        "redex-line-number-map"));
  }

  /**
   * Check that the stack trace of a non-inlined function makes sense.
   */
  @Test
  public void testBasic() throws Exception {
    try {
      InlineSeparateFile.wrapsThrow();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(TraceUtil.traceToString(trace, 2), Arrays.asList(
        "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:12)",
        "com.facebook.redexlinemap.InlineMain.testBasic(InlineMain.java:35)"
      ));
    }
  }

  /**
   * Check the stack trace of a once-inlined function.
   */
  @Test
  public void testInlineOnce() throws Exception {
    try {
      InlineSeparateFile.inlineOnce();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(TraceUtil.traceToString(trace, 3), Arrays.asList(
        "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:12)",
        "com.facebook.redexlinemap.InlineMain.testInlineOnce(InlineSeparateFile.java:16)",
        "com.facebook.redexlinemap.InlineMain.testInlineOnce(InlineMain.java:51)"
      ));
    }
  }

  /**
   * Check the stack trace of a multiply-inlined function.
   */
  @Test
  public void testInlineTwice() throws Exception {
    try {
      InlineSeparateFile.inlineTwice();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(TraceUtil.traceToString(trace, 4), Arrays.asList(
        "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:12)",
        "com.facebook.redexlinemap.InlineMain.testInlineTwice(InlineSeparateFile.java:20)",
        "com.facebook.redexlinemap.InlineMain.testInlineTwice(InlineSeparateFile.java:24)",
        "com.facebook.redexlinemap.InlineMain.testInlineTwice(InlineMain.java:68)"
      ));
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
      assertEquals(TraceUtil.traceToString(trace, 2), Arrays.asList(
       "com.facebook.redexlinemap.InlineMain.ignoreAndThrow(InlineMain.java:86)",
       "com.facebook.redexlinemap.InlineMain.testPositionReset(InlineMain.java:97)"
      ));
    }
  }

  private void elseThrows() throws Exception {
    if (lm == null) {
      System.out.println("");
    } else {
      InlineSeparateFile.wrapsThrow();
    }
  }

  @Test
  public void testElseThrows() throws Exception {
    try {
      elseThrows();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(
          TraceUtil.traceToString(trace, 3),
          Arrays.asList(
            "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:12)",
            "com.facebook.redexlinemap.InlineMain.testElseThrows(InlineMain.java:111)",
            "com.facebook.redexlinemap.InlineMain.testElseThrows(InlineMain.java:118)"));
    }
  }
}
