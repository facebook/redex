/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package com.facebook.redex.test.instr;

import static org.fest.assertions.api.Assertions.*;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

import org.junit.Before;
import org.junit.Test;

public class OutlineThrowTest {
  private static final String MSG_1 = "this is a test";
  private static final String MSG_2 = "this is another test";

  @Before
  public void setup() {
  }

  /**
   * Most basic smoke test.
   */
  @Test(expected = RuntimeException.class)
  public void testThrow() {
    throw new RuntimeException(MSG_1);
  }

  /**
   * Make sure the "right exception" is being thrown
   */
  @Test
  public void testThrowMessage() {
    try {
      thrower();
    } catch (IllegalArgumentException e) {
      // probably redundant w/ check-cast, but no harm
      assertThat(e).isInstanceOf(IllegalArgumentException.class);
      assertThat(e.getMessage()).isEqualTo(MSG_2);
    }
  }

  /**
   * Make sure the dispatcher was actually generated
   */
  @Test
  public void testDispatcherExists() throws Exception {
    // N.B. names need to stay in sync with Outliner.cpp
    Class outlined = Class.forName("com.facebook.redex.Outlined");
    assertThat(outlined).isNotNull();
  }

  /**
   * Poke at the dispatcher in a good way
   */
  @Test(expected = Throwable.class)
  public void testDispatcherValidInvoke() throws Exception {
    // N.B. names need to stay in sync with Outliner.cpp
    Class outlined = Class.forName("com.facebook.redex.Outlined");
    Method dispatch = outlined.getMethod("$dispatch$throws", int.class);
    dispatch.invoke(null, 0);
  }

  /**
   * Poke at the dispatcher in a bad way
   */
  @Test(expected = InvocationTargetException.class)
  public void testDispatcherInvalidInvoke() throws Exception {
    // N.B. names need to stay in sync with Outliner.cpp
    Class outlined = Class.forName("com.facebook.redex.Outlined");
    Method dispatch = outlined.getMethod("$dispatch$throws", int.class);
    dispatch.invoke(null, Integer.MAX_VALUE);
  }

  public void thrower() {
    throw new IllegalArgumentException(MSG_2);
  }
}
