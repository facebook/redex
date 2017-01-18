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
  public void test_Coalesce_InitVoid_AppendString() {
    StringBuilder a, b;
    a = new StringBuilder().append("foobar"); // Match
    b = new StringBuilder("foobar");
    assertThat(a.toString()).isEqualTo(b.toString());
    assertThat(a.toString()).isEqualTo("foobar");
  }

  @Test
  public void test_Coalesce_AppendString_AppendString() {
    StringBuilder a, b;
    a = new StringBuilder();
    a.append("foo").append("bar"); // Match
    assertThat(a.toString()).isEqualTo("foobar");

    b = new StringBuilder();
    // It matches only one time. So does PG.
    b.append("abc").append("def").append("ghi"); // Match
    assertThat(b.toString()).isEqualTo("abcdefghi");
  }

  @Test
  public void test_CompileTime_StringLength() {
    // 4x matches
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
  public void test_Remove_AppendEmptyString() {
    StringBuilder a, b;
    a = new StringBuilder("foo");
    a.append("");
    assertThat(a.toString()).isEqualTo("foo");

    b = new StringBuilder("foo");
    assertThat(b.toString()).isEqualTo("foo");
  }

  @Test
  public void test_Coalesce_Init_AppendChar() {
    StringBuilder a;
    a = new StringBuilder().append('b');
    assertThat(a.toString()).isEqualTo("b");

    a = new StringBuilder().append('\uAE40');
    assertThat(a.toString()).isEqualTo("\uAE40");
  }

  @Test
  public void test_Coalesce_AppendString_AppendInt() {
    StringBuilder a;
    a = new StringBuilder();
    a.append("foo").append(2); // iconst_2, const/4, append:(I)
    assertThat(a.toString()).isEqualTo("foo2");

    a = new StringBuilder();
    a.append("foo").append(8); // bipush, const/16, append:(I)
    assertThat(a.toString()).isEqualTo("foo8");

    a = new StringBuilder();
    a.append("foo").append(42); // bipush, const/16, append:(I)
    assertThat(a.toString()).isEqualTo("foo42");

    a = new StringBuilder();
    a.append("foo").append(32767); // sipush, const/16, append:(I)
    assertThat(a.toString()).isEqualTo("foo32767");

    a = new StringBuilder();
    a.append("foo").append(1234567890); // ldc, const, append:(I)
    assertThat(a.toString()).isEqualTo("foo1234567890");
  }

  @Test
  public void test_Coalesce_AppendString_AppendChar() {
    StringBuilder a;
    a = new StringBuilder();
     // bipush, const/16, append:(C)
    a.append("foo").append('0'); // 0x30
    assertThat(a.toString()).isEqualTo("foo0");

    a = new StringBuilder();
    a.append("foo").append('Z'); // 0x5A
    assertThat(a.toString()).isEqualTo("fooZ");

    a = new StringBuilder();
    a.append("foo").append('\u0660');
    assertThat(a.toString()).isEqualTo("foo\u0660");

    // Not const16, but it uses const, failing to match.
    a = new StringBuilder();
    a.append("foo").append('\uAE40');
    assertThat(a.toString()).isEqualTo("foo\uAE40");
  }

  @Test
  public void test_Coalesce_AppendString_AppendBoolean() {
    StringBuilder a;
    a = new StringBuilder();
    a.append("foo").append(true); // iconst_1, const/4, append:(Z)
    assertThat(a.toString()).isEqualTo("footrue");

    a = new StringBuilder();
    a.append("foo").append(false);
    assertThat(a.toString()).isEqualTo("foofalse");
  }

  @Test
  public void test_Coalesce_AppendString_AppendLongInt() {
    StringBuilder a;
    // ldc2_w, const-wide, (J)
    a = new StringBuilder();
    a.append("foo").append(1234567890123456789L);
    assertThat(a.toString()).isEqualTo("foo1234567890123456789");

    // const-wide/16
    a = new StringBuilder();
    a.append("foo").append(1L);
    assertThat(a.toString()).isEqualTo("foo1");

    // const-wide/32
    a = new StringBuilder();
    a.append("foo").append(12345678L);
    assertThat(a.toString()).isEqualTo("foo12345678");
  }

  @Test
  public void test_CompileTime_StringCompare() {
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
  public void test_Replace_ValueOfBoolean() {
    String a;
    a = String.valueOf(true);
    assertThat(a).isEqualTo("true");
    a = String.valueOf(false);
    assertThat(a).isEqualTo("false");
  }

  @Test
  public void test_Replace_ValueOfChar() {
    String a;
    a = String.valueOf('X');
    assertThat(a).isEqualTo("X");
    a = String.valueOf('\u0660');
    assertThat(a).isEqualTo("\u0660");
    a = String.valueOf('\uAE40');      // Not matched!
    assertThat(a).isEqualTo("\uAE40");
  }

  @Test
  public void test_Replace_ValueOfInt() {
    String a;
    // const/4
    a = String.valueOf(0);
    assertThat(a).isEqualTo("0");
    // const/4
    a = String.valueOf(7);
    assertThat(a).isEqualTo("7");
    // const/4
    a = String.valueOf(-8);
    assertThat(a).isEqualTo("-8");
    // const/16
    a = String.valueOf(32);
    assertThat(a).isEqualTo("32");
    // const/16
    a = String.valueOf(32767);
    assertThat(a).isEqualTo("32767");
    // const/16
    a = String.valueOf(-32768);
    assertThat(a).isEqualTo("-32768");
    // const
    a = String.valueOf(123456789);
    assertThat(a).isEqualTo("123456789");
    // const
    a = String.valueOf(-123456789);
    assertThat(a).isEqualTo("-123456789");
  }

  @Test
  public void test_Replace_ValueOfLongInt() {
    String a;
    // const-wide
    a = String.valueOf(1234567890123456789L);
    assertThat(a).isEqualTo("1234567890123456789");
    // const-wide/16
    a = String.valueOf(1L);
    assertThat(a).isEqualTo("1");
    // const-wide/32
    a = String.valueOf(12345678L);
    assertThat(a).isEqualTo("12345678");
  }

  @Test
  public void test_Replace_ValueOfFloat() {
    String a;
    a = String.valueOf((float) 3.141593f);
    assertThat(a).isEqualTo("3.141593");
  }

  @Test
  public void test_Replace_ValueOfDouble() {
    String a;
    // const-wide
    a = String.valueOf((double)3.141592653589793d);
    assertThat(a.startsWith("3.14")).isTrue();
    // const-wide/16
    a = String.valueOf((double)0d);
    assertThat(a.startsWith("0.0")).isTrue();
    // const-wide/high16: we don't handle this case for now.
    a = String.valueOf((double)8d);
    assertThat(a).isEqualTo("8.0");
  }
}
