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

import android.content.Context;
import android.support.test.InstrumentationRegistry;
import com.facebook.soloader.SoLoader;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

import org.junit.Before;
import org.junit.Test;

public class NativeOutlinerTest {
  @Before
  public void setup() throws Exception {
    Context context = InstrumentationRegistry.getTargetContext();
    SoLoader.init(context, 0);
  }

  /**
   * Most basic smoke test.
   */
  @Test(expected = RuntimeException.class)
  public void testThrow() {
    throw new RuntimeException("this is a test");
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
      // derpy way of comparing the string without keeping the string around
      assertThat(e.getMessage()).startsWith("this is ");
      assertThat(e.getMessage()).endsWith(" another test");
    }
  }

  /**
   * Make sure the dispatcher was actually generated
   */
  @Test
  public void testDispatcherExists() throws Exception {
    // N.B. names need to stay in sync with NativeOutliner.cpp
    Class outlined = Class.forName("com.facebook.redex.NativeOutlined");
    assertThat(outlined).isNotNull();
  }

  /**
   * Poke at the dispatcher in a good way
   */
  @Test(expected = Throwable.class)
  public void testDispatcherValidInvoke() throws Exception {
    // N.B. names need to stay in sync with NativeOutliner.cpp
    Class outlined = Class.forName("com.facebook.redex.NativeOutlined");
    Method dispatch = outlined.getMethod("$dispatch$throws", int.class);
    dispatch.invoke(null, 0);
  }

  /**
   * Poke at the dispatcher in a bad way
   */
  @Test(expected = InvocationTargetException.class)
  public void testDispatcherInvalidInvoke() throws Exception {
    // N.B. names need to stay in sync with Outliner.cpp
    Class outlined = Class.forName("com.facebook.redex.NativeOutlined");
    Method dispatch = outlined.getMethod("$dispatch$throws", int.class);
    dispatch.invoke(null, Integer.MAX_VALUE);
  }

  public void thrower() {
    throw new IllegalArgumentException("this is another test");
  }
}
