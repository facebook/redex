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
       "com.facebook.redexlinemap.DisabledLineMapTest.wrapsThrow(DisabledLineMapTest.java:22)",
       "com.facebook.redexlinemap.DisabledLineMapTest.testStackTraceWithoutLineMap(DisabledLineMapTest.java:26)"));
    }
  }
}
