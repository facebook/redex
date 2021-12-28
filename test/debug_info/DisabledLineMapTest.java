/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap;

import static org.fest.assertions.api.Assertions.*;

import java.util.Arrays;
import java.util.List;
import org.junit.Test;

public class DisabledLineMapTest {

  @NoInline
  private void wrapsThrow() throws Exception {
    throw new Exception("foo");
  }

  private void inlinedThrower() throws Exception {
    wrapsThrow();
  }

  @Test
  public void testStackTraceWithoutLineMap() {
    try {
      inlinedThrower();
    } catch (Exception e) {
      List<StackTraceElement> trace = Arrays.asList(e.getStackTrace());
      assertThat(TraceUtil.traceToString(trace, 2)).isEqualTo(Arrays.asList(
       "com.facebook.redexlinemap.DisabledLineMapTest.wrapsThrow(DisabledLineMapTest.java:20)",
       "com.facebook.redexlinemap.DisabledLineMapTest.testStackTraceWithoutLineMap(DisabledLineMapTest.java:24)"));
    }
  }
}
