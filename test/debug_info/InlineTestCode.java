/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

import static org.fest.assertions.api.Assertions.assertThat;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;

public class InlineTestCode {

  LineMapperV2 lm;

  public InlineTestCode(LineMapperV2 lm) {
    this.lm = lm;
  }

  /** Check that the stack trace of a non-inlined function makes sense. */
  @NoInline
  @SuppressWarnings("CatchGeneralException")
  public void testBasic() throws Exception {
    try {
      InlineSeparateFileV2.wrapsThrow();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 2))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineTestCode.testBasic(InlineTestCode.java:30)"));
    }
  }

  /** Check the stack trace of a once-inlined function. */
  @NoInline
  @SuppressWarnings("CatchGeneralException")
  public void testInlineOnce() throws Exception {
    try {
      InlineSeparateFileV2.inlineOnce();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.inlineOnce(InlineSeparateFileV2.java:16)",
                  "com.facebook.redexlinemap.InlineTestCode.testInlineOnce(InlineTestCode.java:46)"));
    }
  }

  /** Check the stack trace of a multiply-inlined function. */
  @NoInline
  @SuppressWarnings("CatchGeneralException")
  public void testInlineTwice() throws Exception {
    try {
      InlineSeparateFileV2.inlineTwice();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 4))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.inlineOnce1(InlineSeparateFileV2.java:20)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.inlineTwice(InlineSeparateFileV2.java:24)",
                  "com.facebook.redexlinemap.InlineTestCode.testInlineTwice(InlineTestCode.java:63)"));
    }
  }

  private static int inlineMe() {
    return 1;
  }

  @NoInline
  private void ignoreAndThrow(Object x) throws Exception {
    throw new RuntimeException("foo");
  }

  /**
   * Check that the line number is reset to the original for the code that follows an inlined
   * method. We expect the resulting stack trace *not* to point to inlineMe().
   */
  @NoInline
  @SuppressWarnings("CatchGeneralException")
  public void testPositionReset() throws Exception {
    try {
      ignoreAndThrow(inlineMe());
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 2))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineTestCode.ignoreAndThrow(InlineTestCode.java:82)",
                  "com.facebook.redexlinemap.InlineTestCode.testPositionReset(InlineTestCode.java:93)"));
    }
  }

  private void elseThrows() throws Exception {
    if (lm == null) {
      System.out.println("");
    } else {
      InlineSeparateFileV2.wrapsThrow();
    }
  }

  @NoInline
  @SuppressWarnings("CatchGeneralException")
  public void testElseThrows() throws Exception {
    try {
      elseThrows();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineTestCode.elseThrows(InlineTestCode.java:108)",
                  "com.facebook.redexlinemap.InlineTestCode.testElseThrows(InlineTestCode.java:116)"));
    }
  }

  public static void checkTests(Class<?> cls) throws Exception {
    HashSet<String> expected = getTestNamedMethods(InlineTestCode.class);
    HashSet<String> actual = getTestNamedMethods(cls);
    assertThat(actual).isEqualTo(expected);
  }

  private static HashSet<String> getTestNamedMethods(Class<?> cls) throws Exception {
    HashSet<String> res = new HashSet<>();
    for (Method m : cls.getDeclaredMethods()) {
      String name = m.getName();
      if (name.startsWith("test")) {
        res.add(name);
      }
    }
    return res;
  }
}
