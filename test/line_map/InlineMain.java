/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
        "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:14)",
        "com.facebook.redexlinemap.InlineMain.testBasic(InlineMain.java:37)"
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
        "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:14)",
        "com.facebook.redexlinemap.InlineMain.testInlineOnce(InlineSeparateFile.java:18)",
        "com.facebook.redexlinemap.InlineMain.testInlineOnce(InlineMain.java:53)"
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
        "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:14)",
        "com.facebook.redexlinemap.InlineMain.testInlineTwice(InlineSeparateFile.java:22)",
        "com.facebook.redexlinemap.InlineMain.testInlineTwice(InlineSeparateFile.java:26)",
        "com.facebook.redexlinemap.InlineMain.testInlineTwice(InlineMain.java:70)"
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
       "com.facebook.redexlinemap.InlineMain.ignoreAndThrow(InlineMain.java:88)",
       "com.facebook.redexlinemap.InlineMain.testPositionReset(InlineMain.java:99)"
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
            "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:14)",
            "com.facebook.redexlinemap.InlineMain.testElseThrows(InlineMain.java:113)",
            "com.facebook.redexlinemap.InlineMain.testElseThrows(InlineMain.java:120)"));
    }
  }
}
