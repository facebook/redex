/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

import static org.fest.assertions.api.Assertions.*;

import java.util.Arrays;
import java.util.List;
import org.junit.Test;

public class DisabledLineMapOutliningTest {

  @NoInline
  public static void wrapsThrow() throws Exception {
    throw new Exception("foo");
  }

  @NoInline
  public static void outlinedThrower() throws Exception {
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
    wrapsThrow();
  }

  @Test
  public void testStackTraceWithoutLineMap() {
    try {
      outlinedThrower();
    } catch (Exception e) {
      // The $outlined$ method was sandwiched in.
      // It resides in a newly created helper class, as the test class itself
      // is marked as not renamable, and thus is not eligible for hosting an
      // outlined helper method.
      // The long number is a stable hash of the outlined instruction sequence.
      List<StackTraceElement> trace = Arrays.asList(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 3)).isEqualTo(Arrays.asList(
       "com.facebook.redexlinemap.DisabledLineMapOutliningTest.wrapsThrow(DisabledLineMapOutliningTest.java:20)",
       "com.redex.Outlined$0$0$0.$outlined$0$626959d2b9a059a0(Unknown Source)",
       "com.facebook.redexlinemap.DisabledLineMapOutliningTest.outlinedThrower(DisabledLineMapOutliningTest.java:25)"));
    }
  }
}
