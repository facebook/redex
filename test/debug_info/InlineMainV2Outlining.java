/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

import static org.assertj.core.api.Assertions.assertThat;

import java.util.ArrayList;
import java.util.Arrays;
import org.junit.Before;
import org.junit.BeforeClass;
import org.junit.Test;

public class InlineMainV2Outlining {

  LineMapperV2 lm;
  InlineTestCode itc;

  @BeforeClass
  public static void checkTests() throws Exception {
    InlineTestCode.checkTests(InlineMainV2Outlining.class);
  }

  @Before
  public void setUp() throws Exception {
    lm = new LineMapperV2(getClass().getResourceAsStream("/assets/redex-line-number-map-v2"));
    itc = new InlineTestCode(lm);
  }

  /** Check that the stack trace of a non-inlined function makes sense. */
  @Test
  public void testBasic() throws Exception {
    itc.testBasic();
  }

  /** Check the stack trace of a once-inlined function. */
  @Test
  public void testInlineOnce() throws Exception {
    itc.testInlineOnce();
  }

  /** Check the stack trace of a multiply-inlined function. */
  @Test
  public void testInlineTwice() throws Exception {
    itc.testInlineTwice();
  }

  @Test
  public void testPositionReset() throws Exception {
    itc.testPositionReset();
  }

  @Test
  public void testElseThrows() throws Exception {
    itc.testElseThrows();
  }

  @Test
  public void testElseThrowsOverload() throws Exception {
    itc.testElseThrowsOverload(null);
  }

  @Test
  @NoInline
  @SuppressWarnings("CatchGeneralException")
  public void testOutlined() throws Exception {
    try {
      InlineSeparateFileV2.counter = 1;
      InlineSeparateFileV2.outlinedThrower();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow2(InlineSeparateFileV2.java:32)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.outlinedThrower(InlineSeparateFileV2.java:38)",
                  // "com.redex.Outlined$0$0$0.$outlined$0$6dde6ce7db5fb2e0(RedexGenerated)",
                  "com.facebook.redexlinemap.InlineMainV2Outlining.testOutlined(InlineMainV2Outlining.java:73)"));
    }

    try {
      InlineSeparateFileV2.counter = 2;
      InlineSeparateFileV2.outlinedThrower();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow2(InlineSeparateFileV2.java:32)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.outlinedThrower(InlineSeparateFileV2.java:39)",
                  // "com.redex.Outlined$0$0$0.$outlined$0$6dde6ce7db5fb2e0(RedexGenerated)",
                  "com.facebook.redexlinemap.InlineMainV2Outlining.testOutlined(InlineMainV2Outlining.java:87)"));
    }

    try {
      InlineSeparateFileV2.counter = 3;
      InlineSeparateFileV2.outlinedThrower();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow2(InlineSeparateFileV2.java:32)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.outlinedThrower(InlineSeparateFileV2.java:40)",
                  // "com.redex.Outlined$0$0$0.$outlined$0$6dde6ce7db5fb2e0(RedexGenerated)",
                  "com.facebook.redexlinemap.InlineMainV2Outlining.testOutlined(InlineMainV2Outlining.java:101)"));
    }

    try {
      InlineSeparateFileV2.counter = 42;
      InlineSeparateFileV2.outlinedThrower();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow2(InlineSeparateFileV2.java:32)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.outlinedThrower(InlineSeparateFileV2.java:79)",
                  // "com.redex.Outlined$0$0$0.$outlined$0$6dde6ce7db5fb2e0(RedexGenerated)",
                  "com.facebook.redexlinemap.InlineMainV2Outlining.testOutlined(InlineMainV2Outlining.java:115)"));
    }
  }

  @Test
  @NoInline
  @SuppressWarnings("CatchGeneralException")
  public void testOutlinedInlined() throws Exception {
    try {
      InlineSeparateFileV2.counter = 1;
      InlineSeparateFileV2.outlinedThrowerInlined();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 4))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow3(InlineSeparateFileV2.java:91)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrowInline(InlineSeparateFileV2.java:97)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.outlinedThrowerInlined(InlineSeparateFileV2.java:102)",
                  // "com.redex.Outlined$0$0$0.$outlined$0$bfeec5cff070a818(RedexGenerated)",
                  "com.facebook.redexlinemap.InlineMainV2Outlining.testOutlinedInlined(InlineMainV2Outlining.java:134)"));
    }

    try {
      InlineSeparateFileV2.counter = 2;
      InlineSeparateFileV2.outlinedThrowerInlined();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 4))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow3(InlineSeparateFileV2.java:91)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrowInline(InlineSeparateFileV2.java:97)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.outlinedThrowerInlined(InlineSeparateFileV2.java:103)",
                  // "com.redex.Outlined$0$0$0.$outlined$0$bfeec5cff070a818(RedexGenerated)",
                  "com.facebook.redexlinemap.InlineMainV2Outlining.testOutlinedInlined(InlineMainV2Outlining.java:149)"));
    }

    try {
      InlineSeparateFileV2.counter = 3;
      InlineSeparateFileV2.outlinedThrowerInlined();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 4))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow3(InlineSeparateFileV2.java:91)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrowInline(InlineSeparateFileV2.java:97)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.outlinedThrowerInlined(InlineSeparateFileV2.java:104)",
                  // "com.redex.Outlined$0$0$0.$outlined$0$bfeec5cff070a818(RedexGenerated)",
                  "com.facebook.redexlinemap.InlineMainV2Outlining.testOutlinedInlined(InlineMainV2Outlining.java:164)"));
    }

    try {
      InlineSeparateFileV2.counter = 42;
      InlineSeparateFileV2.outlinedThrowerInlined();
    } catch (Exception e) {
      ArrayList<StackTraceElement> trace = lm.mapStackTrace(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 4))
          .isEqualTo(
              Arrays.asList(
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrow3(InlineSeparateFileV2.java:91)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.wrapsThrowInline(InlineSeparateFileV2.java:97)",
                  "com.facebook.redexlinemap.InlineSeparateFileV2.outlinedThrowerInlined(InlineSeparateFileV2.java:143)",
                  // "com.redex.Outlined$0$0$0.$outlined$0$bfeec5cff070a818(RedexGenerated)",
                  "com.facebook.redexlinemap.InlineMainV2Outlining.testOutlinedInlined(InlineMainV2Outlining.java:179)"));
    }
  }

  @Test
  public void testKotlinPrecondition() throws Exception {
    itc.testKotlinPrecondition();
  }
}
