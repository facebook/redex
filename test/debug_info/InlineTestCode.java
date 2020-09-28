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
      assertRawStackSize(e, 2, "testBasic");
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
      assertRawStackSize(e, 2, "testInlineOnce");
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.inlineOnce(InlineSeparateFileV2.java:16)",
                  "com.facebook.redexlinemap.InlineTestCode.testInlineOnce(InlineTestCode.java:47)"));
    }
  }

  /** Check the stack trace of a multiply-inlined function. */
  @NoInline
  @SuppressWarnings("CatchGeneralException")
  public void testInlineTwice() throws Exception {
    try {
      InlineSeparateFileV2.inlineTwice();
    } catch (Exception e) {
      assertRawStackSize(e, 2, "testInlineTwice");
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 4))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.inlineOnce1(InlineSeparateFileV2.java:20)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.inlineTwice(InlineSeparateFileV2.java:24)",
                  "com.facebook.redexlinemap.InlineTestCode.testInlineTwice(InlineTestCode.java:65)"));
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
      assertRawStackSize(e, 2, "testPositionReset");
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 2))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineTestCode.ignoreAndThrow(InlineTestCode.java:85)",
                  "com.facebook.redexlinemap.InlineTestCode.testPositionReset(InlineTestCode.java:96)"));
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
      assertRawStackSize(e, 2, "testElseThrows");
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineTestCode.elseThrows(InlineTestCode.java:112)",
                  "com.facebook.redexlinemap.InlineTestCode.testElseThrows(InlineTestCode.java:120)"));
    }
  }

  @NoInline
  private void elseThrows(int i) throws Exception {
    if (lm == null) {
      System.out.println(i);
    } else {
      InlineSeparateFileV2.wrapsThrow();
    }
  }

  @NoInline
  private void elseThrows(char c) throws Exception {
    if (lm != null) {  // Intentionally changed order.
      InlineSeparateFileV2.wrapsThrow();
    } else {
      System.out.println(c);
    }
  }

  @NoInline
  private void elseThrows(float f) throws Exception {
    if (lm == null) {
      System.out.println(f);
    } else {
      InlineSeparateFileV2.wrapsThrow();
    }
  }

  @NoInline
  private void elseThrows(Object o) throws Exception {
    if (lm != null) {  // Intentionally changed order.
      InlineSeparateFileV2.wrapsThrow();
    } else {
      System.out.println(o);
    }
  }

  @NoInline
  @SuppressWarnings("CatchGeneralException")
  public void testElseThrowsOverload() throws Exception {
    HashSet<StackTraceElement> frames = new HashSet<>();
    try {
      elseThrows((int)0);
    } catch (Exception e) {
      assertRawStackSize(e, 3, "testElseThrowsOverload");
      frames.add(e.getStackTrace()[1]);
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineTestCode.elseThrows(InlineTestCode.java:138)",
                  "com.facebook.redexlinemap.InlineTestCode.testElseThrowsOverload(InlineTestCode.java:174)"));
    }

    try {
      elseThrows('a');
    } catch (Exception e) {
      assertRawStackSize(e, 3, "testElseThrowsOverload");
      frames.add(e.getStackTrace()[1]);
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineTestCode.elseThrows(InlineTestCode.java:145)",
                  "com.facebook.redexlinemap.InlineTestCode.testElseThrowsOverload(InlineTestCode.java:188)"));
    }

    try {
      elseThrows(0.1f);
    } catch (Exception e) {
      assertRawStackSize(e, 3, "testElseThrowsOverload");
      frames.add(e.getStackTrace()[1]);
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineTestCode.elseThrows(InlineTestCode.java:156)",
                  "com.facebook.redexlinemap.InlineTestCode.testElseThrowsOverload(InlineTestCode.java:202)"));
    }

    try {
      elseThrows(null);
    } catch (Exception e) {
      assertRawStackSize(e, 3, "testElseThrowsOverload");
      frames.add(e.getStackTrace()[1]);
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow(InlineSeparateFileV2.java:12)",
                  "com.facebook.redexlinemap.InlineTestCode.elseThrows(InlineTestCode.java:163)",
                  "com.facebook.redexlinemap.InlineTestCode.testElseThrowsOverload(InlineTestCode.java:216)"));
    }

    // Check that all `elseThrows` functions exist (not inlined).
    HashSet<Method> methods = new HashSet<>();
    for (Method m : getClass().getDeclaredMethods()) {
      if (m.getName().equals("elseThrows")) {
        methods.add(m);
      }
    }
    assertThat(methods).hasSize(4);
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

  private static void assertRawStackSize(Throwable t, int size, String expectedMethod) {
    assertRawStackSize(t, size, expectedMethod, InlineTestCode.class.getName());
  }
  private static void assertRawStackSize(Throwable t, int size, String expectedMethod, String expectedClass) {
    StackTraceElement[] trace = t.getStackTrace();
    assertThat(trace.length).isGreaterThanOrEqualTo(size);
    assertThat(trace[size - 1].getMethodName()).isEqualTo(expectedMethod);
    if (expectedClass != null) {
      assertThat(trace[size - 1].getClassName()).endsWith(expectedClass);
    }
  }
}
