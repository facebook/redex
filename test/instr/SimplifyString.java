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

import org.junit.Before;
import org.junit.Test;

public class SimplifyString {

  @Test
  public void testCase1() {
    Boolean a;
    a = "".equals("");
    assertThat(a).isTrue();
    a = "abc".equals("abc");
    assertThat(a).isTrue();
    a = "abc".equals("xyz");
    assertThat(a).isFalse();
    a = "abc".equals("abcd");
    assertThat(a).isFalse();
  }

  @Test
  public void testCase2() {
    int a;
    a = "".length();
    assertThat(a).isEqualTo(0);
    a = "abc".length();
    assertThat(a).isEqualTo(3);
    a = "abcdef".length();
    assertThat(a).isEqualTo(6);

    // https://phabricator.intern.facebook.com/diffusion/FBS/browse/master/fbandroid/third-party/java/guava/guava-gwt/src-super/com/google/common/base/super/com/google/common/base/CharMatcher.java
    // private static final class Digit extends RangesMatcher
    a = ("0\u0660\u06f0\u07c0\u0966\u09e6\u0a66\u0ae6\u0b66\u0be6\u0c66\u0ce6\u0d66\u0e50\u0ed0\u0f20\u1040\u1090\u17e0\u1810\u1946\u19d0\u1b50\u1bb0\u1c40\u1c50\ua620\ua8d0\ua900\uaa50\uff10").length();
    assertThat(a).isEqualTo(31);
  }

  @Test
  public void testCase3to9() {
    String a;
    a = String.valueOf(true);                       // 3
    assertThat(a).isEqualTo("true");
    a = String.valueOf(false);                      // 3
    assertThat(a).isEqualTo("false");
    a = String.valueOf('X');                        // 4
    assertThat(a).isEqualTo("X");
    a = String.valueOf('\uAE40');                   // Ignore such case
    assertThat(a).isEqualTo("\uAE40");
    a = String.valueOf(42);                         // 5
    assertThat(a).isEqualTo("42");
    a = String.valueOf(255);                        // 5
    assertThat(a).isEqualTo("255");
    a = String.valueOf(12345678);                   // 6
    assertThat(a).isEqualTo("12345678");
    a = String.valueOf(1234567890123456789L);       // 7
    assertThat(a).isEqualTo("1234567890123456789");
    a = String.valueOf((float)3.141593f);           // 8
    assertThat(a).isEqualTo("3.141593");
    a = String.valueOf((double)3.141592653589793d); // 9
    assertThat(a.startsWith("3.14")).isTrue();
  }
}
