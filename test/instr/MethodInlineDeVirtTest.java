/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexinline;

import static org.fest.assertions.api.Assertions.*;
import android.util.Log;
import android.os.Build;
import android.annotation.TargetApi;
import com.facebook.annotations.*;

import org.junit.Test;

final class MethodInlineTest {
  public String getHello() {
    return "Hello";
  }

  @DoNotDevirtualize
  public String addWorld() {
    return " World";
  }
}

public class MethodInlineDeVirtTest {
  @Test
  public void testInvoke() {
    MethodInlineTest m = new MethodInlineTest();
    assertThat(m.getHello() + m.addWorld()).isEqualTo("Hello World");
  }
}

