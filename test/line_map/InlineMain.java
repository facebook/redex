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
        "redex-line-number-map.txt"));
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
      assertEquals(TraceUtil.traceToString(trace, 4), Arrays.asList(
        "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:14)",
        "com.facebook.redexlinemap.InlineSeparateFile.inlineOnce(InlineSeparateFile.java:18)",
        "com.facebook.redexlinemap.InlineSeparateFile.inlineOnce(InlineSeparateFile.java:22)",
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
      assertEquals(TraceUtil.traceToString(trace, 5), Arrays.asList(
        "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:14)",
        "com.facebook.redexlinemap.InlineSeparateFile.inlineTwice(InlineSeparateFile.java:26)",
        "com.facebook.redexlinemap.InlineSeparateFile.inlineTwice(InlineSeparateFile.java:30)",
        "com.facebook.redexlinemap.InlineSeparateFile.inlineTwice(InlineSeparateFile.java:34)",
        "com.facebook.redexlinemap.InlineMain.testInlineTwice(InlineMain.java:71)"
      ));
    }
  }

  private static int inlineMe() {
    return 1;
  }

  private void ignoreAndThrow(Object x) throws Exception {
    throw new Exception("foo");
  }

  private void callIgnoreAndThrow() throws Exception {
    ignoreAndThrow(inlineMe());
  }

  /**
   * Check that the line number is reset to the original for the code that
   * follows an inlined method. We expect the resulting stack trace *not*
   * to point to inlineMe().
   *
   * Note that we don't call ignoreAndThrow() directly because the try-catch
   * block here prevents inlining from occurring.
   */
  @Test
  public void testPositionReset() throws Exception {
    try {
      callIgnoreAndThrow();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(TraceUtil.traceToString(trace, 3), Arrays.asList(
       "com.facebook.redexlinemap.InlineMain.ignoreAndThrow(InlineMain.java:89)",
       "com.facebook.redexlinemap.InlineMain.callIgnoreAndThrow(InlineMain.java:93)",
       "com.facebook.redexlinemap.InlineMain.testPositionReset(InlineMain.java:107)"
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

  private void callElseThrows() throws Exception {
    elseThrows();
  }

  @Test
  public void testElseThrows() throws Exception {
    try {
      callElseThrows();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertEquals(
          TraceUtil.traceToString(trace, 4),
          Arrays.asList(
            "com.facebook.redexlinemap.InlineSeparateFile.wrapsThrow(InlineSeparateFile.java:14)",
            "com.facebook.redexlinemap.InlineMain.callElseThrows(InlineMain.java:122)",
            "com.facebook.redexlinemap.InlineMain.callElseThrows(InlineMain.java:127)",
            "com.facebook.redexlinemap.InlineMain.testElseThrows(InlineMain.java:133)"));
    }
  }
}
