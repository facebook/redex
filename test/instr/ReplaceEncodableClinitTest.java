/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

package redex;

import static org.fest.assertions.api.Assertions.*;
import org.junit.Test;

public class ReplaceEncodableClinitTest {
  @Test
  public void test() {
    assertThat(Encodable.S_BOOL).isEqualTo(true);
    assertThat(Encodable.S_BYTE).isEqualTo((byte) 'b');
    assertThat(Encodable.S_CHAR).isEqualTo('c');
    assertThat(Encodable.S_SHORT).isEqualTo((short) 128);
    assertThat(Encodable.S_INT).isEqualTo(12345);
    assertThat(Encodable.S_STRING).isEqualTo("string");
    assertThat(Encodable.S_LONG).isEqualTo(0x1000200030004000L);
    assertThat(Encodable.S_DOUBLE).isEqualTo(1.0000000000000002);
  }
}

class Encodable {
  public static boolean S_BOOL = true;
  public static byte S_BYTE = 'b';
  public static char S_CHAR = 'c';
  public static short S_SHORT = 128;
  public static int S_INT = 12345;
  public static String S_STRING = "string";
  public static long S_LONG = 0x1000200030004000L;
  public static double S_DOUBLE = 1.0000000000000002;
}

class UnEncodable {
  public static final int S_INT = Math.random() > .5 ? 1 : 0;
}

class HasCharSequence {
  // CharSequence must not be processed because DalvikVM can't handle it.
  public static CharSequence S_CHARSEQ = "SEQ";
}
