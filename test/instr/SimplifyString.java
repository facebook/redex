/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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

  private int string_hash_code(String s) {
    // Since `s` is a variable, `String.hashCode()` will not be optimized away
    return s.hashCode();
  }

  @Test
  public void test_CompileTime_StringHashCode() {
    assertThat("".hashCode())
      .isEqualTo(string_hash_code(""));
    assertThat("Redex".hashCode())
      .isEqualTo(string_hash_code("Redex"));
    assertThat("IAmAVeryLongString".hashCode())
      .isEqualTo(string_hash_code("IAmAVeryLongString"));
    assertThat("ABC\u00ea\u1234\u03bb".hashCode())
      .isEqualTo(string_hash_code("ABC\u00ea\u1234\u03bb"));
    assertThat("ABC\u00ea\u1234\u03bbDEF".hashCode())
      .isEqualTo(string_hash_code("ABC\u00ea\u1234\u03bbDEF"));
  }

  @Test
  public void test_Remove_AppendEmptyString() {
    StringBuilder a, b;
    a = new StringBuilder("foo");
    a.append("");
    assertThat(a.toString()).isEqualTo("foo");
  }

  @Test
  public void test_Coalesce_Init_AppendChar() {
    StringBuilder a;
    // https://developer.android.com/reference/java/io/DataInput.html#modified-utf-8
    // Single-byte char [0x0001, 0x007F]: const/16, vA, #int 98 # 0x62
    a = new StringBuilder().append('b');
    assertThat(a.toString()).isEqualTo("b");
    // 2-byte \u0000: const/4, vA, #int 0
    a = new StringBuilder().append('\u0000');
    assertThat(a.toString()).isEqualTo("\u0000");
    // 2-byte char [0x0080, 0x07FF]: const/16, vA, #int 1632 # 0x660
    a = new StringBuilder().append('\u0660');
    assertThat(a.toString()).isEqualTo("\u0660");
    // 3-byte char [0x08000, 0xFFFF]: const, vA, #int AE40
    a = new StringBuilder().append('\uAE40');
    assertThat(a.toString()).isEqualTo("\uAE40");
  }

  @Test
  public void test_Coalesce_AppendString_AppendInt() {
    StringBuilder a;
    // iconst_2, const/4, append:(I)
    a = new StringBuilder();
    a.append("foo").append(2);
    assertThat(a.toString()).isEqualTo("foo2");
    // bipush, const/16, append:(I)
    a = new StringBuilder();
    a.append("foo").append(8);
    assertThat(a.toString()).isEqualTo("foo8");
    // bipush, const/16, append:(I)
    a = new StringBuilder();
    a.append("foo").append(42);
    assertThat(a.toString()).isEqualTo("foo42");
    // sipush, const/16, append:(I)
    a = new StringBuilder();
    a.append("foo").append(32767);
    assertThat(a.toString()).isEqualTo("foo32767");
    // ldc, const, append:(I)
    a = new StringBuilder();
    a.append("foo").append(1234567890);
    assertThat(a.toString()).isEqualTo("foo1234567890");
    // Concatenation with UTF
    a = new StringBuilder();
    a.append("\uABCD\u0ABC").append(987654321);
    // Why substring(0, 4) instead substring(0, 2)? If so, the literal 2 will
    // be factored out with the above the usage of 2. It prevents our poor
    // peephole optimizer from detecting the pattern.
    assertThat(a.toString().substring(0, 4)).isEqualTo("\uABCD\u0ABC98");
    assertThat(a.toString().substring(4)).isEqualTo("7654321");
  }

  @Test
  public void test_Coalesce_AppendString_AppendChar() {
    StringBuilder a;
    // bipush, const/16, append:(C)
    a = new StringBuilder();
    a.append("foo").append('0'); // 0x30
    assertThat(a.toString()).isEqualTo("foo0");
    a = new StringBuilder();
    a.append("foo").append('Z'); // 0x5A
    assertThat(a.toString()).isEqualTo("fooZ");
    // 2-byte char, using const/16
    a = new StringBuilder();
    a.append("foo").append('\u0660');
    assertThat(a.toString()).isEqualTo("foo\u0660");
    // 2-byte \u0000, but it uses const/4
    a = new StringBuilder();
    a.append("foo").append('\u0000');
    assertThat(a.toString()).isEqualTo("foo\u0000");
    // 3-byte char, using const
    a = new StringBuilder();
    a.append("foo").append('\uAE40');
    assertThat(a.toString()).isEqualTo("foo\uAE40");
    // Concatenation with UTF
    a = new StringBuilder();
    a.append("\uABCD\u0ABC").append('x');
    assertThat(a.toString()).startsWith("\uABCD\u0ABC");
    assertThat(a.toString().substring(2)).isEqualTo("x");
  }

  @Test
  public void test_Coalesce_AppendString_AppendBoolean() {
    StringBuilder a;
    a = new StringBuilder();
    a.append("foo").append(true); // iconst_1, const/4, append:(Z)
    assertThat(a.toString()).isEqualTo("footrue");
    a = new StringBuilder();
    a.append("\uABCD\u0ABC").append(false);
    assertThat(a.toString()).startsWith("\uABCD\u0ABC");
    assertThat(a.toString().substring(2)).isEqualTo("false");
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
    // Concatenation with UTF
    a = new StringBuilder();
    a.append("\uABCD\u0ABC").append(424242424242L);
    assertThat(a.toString()).startsWith("\uABCD\u0ABC");
    assertThat(a.toString().substring(2)).isEqualTo("424242424242");
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
    // Single byte, const/16
    a = String.valueOf('X');
    assertThat(a).isEqualTo("X");
    // Two bytes, const/16
    a = String.valueOf('\u0660');
    assertThat(a).isEqualTo("\u0660");
    // Two bytes, const/16, \u0000
    a = String.valueOf('\u0000');
    assertThat(a).isEqualTo("\u0000");
    // Three bytes, const
    a = String.valueOf('\uAE40');
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
    // const-wide
    a = String.valueOf(-1234567890123456789L);
    assertThat(a).isEqualTo("-1234567890123456789");
    // const-wide/16
    a = String.valueOf(0L);
    assertThat(a).isEqualTo("0");
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
    // const v0, #float 3.141593
    a = String.valueOf((float) 3.141593f);
    assertThat(a).isEqualTo("3.141593");
    // const/4 v0, #int 0
    a = String.valueOf((float) 0f);
    assertThat(a.startsWith("0.0")).isTrue();
    // const/high16 v0, #int 0x3f80: we don't handle for now.
    a = String.valueOf((float) 1f);
    assertThat(a.startsWith("1.0")).isTrue();
    // const/high16 v0, #int 0x4000: we don't handle.
    a = String.valueOf((float) 2f);
    assertThat(a.startsWith("2.0")).isTrue();
    // const/high16 v0, #int 0x4100: we don't handle.
    a = String.valueOf((float) 8f);
    assertThat(a.startsWith("8.0")).isTrue();
    // const/high16 v0, #int 0x3c80 = 1/64: we don't handle.
    a = String.valueOf((float) 0.015625f);
    assertThat(a).isEqualTo("0.015625");
    // const v0, #float 1024.125000 // #44800400
    a = String.valueOf((float) 1024.125f);
    assertThat(a.startsWith("1024.125")).isTrue();
}

  @Test
  public void test_Replace_ValueOfDouble() {
    String a;
    // const-wide v0, #double 3.141593 // #400921fb54442d18
    a = String.valueOf((double) 3.141592653589793d);
    assertThat(a.startsWith("3.14")).isTrue();
    // const-wide/16 v0, #int 0
    a = String.valueOf((double) 0d);
    assertThat(a.startsWith("0.0")).isTrue();
    // const-wide/high16 v0, #long 0x3ff0: we don't handle.
    a = String.valueOf((double) 1d);
    assertThat(a.startsWith("1.0")).isTrue();
    // const-wide/high16 v0, #long 0x400: we don't handle.
    a = String.valueOf((double) 2d);
    assertThat(a.startsWith("2.0")).isTrue();
    // const-wide/high16 v0, #long 0x4020: we dont' handle.
    a = String.valueOf((double) 8d);
    assertThat(a.startsWith("8.0")).isTrue();
    // const-wide/high16 v0, #long 0x3f90: we don't handle.
    a = String.valueOf((double) 0.015625d);
    assertThat(a).isEqualTo("0.015625");
    // const-wide v0, #double 1024.125000 // #4090008000000000
    a = String.valueOf((double) 1024.125d);
    assertThat(a.startsWith("1024.125")).isTrue();
  }
}
