/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.*;
import org.junit.Test;

public class FinalInlineV2Test {
  @Test
  public void testFinalInline() {
    assertThat(Encodable.S_BOOL).isEqualTo(true);
    assertThat(Encodable.S_BYTE).isEqualTo((byte) 'b');
    assertThat(Encodable.S_CHAR).isEqualTo('c');
    assertThat(Encodable.S_SHORT).isEqualTo((short) 128);
    assertThat(Encodable.S_INT).isEqualTo(12345);
    assertThat(Encodable.S_STRING).isEqualTo("string");
    assertThat(Encodable.S_LONG).isEqualTo(0x1000200030004000L);
    assertThat(Encodable.S_DOUBLE).isEqualTo(1.0000000000000002);
    assertThat(Encodable.S_FLOAT).isEqualTo(-2.0f);
    assertThat(FinalInlineV2ClinitReadAndWriteTest.getLength()).isEqualTo(3);
  }
}

class Encodable {
  // Don't mark the fields final here so that javac or d8 won't do the inlining.
  // Use AccessMarkingPass to set them to final (because there are no writes).
  public static boolean S_BOOL = true;
  public static byte S_BYTE = 'b';
  public static char S_CHAR = 'c';
  public static short S_SHORT = 128;
  public static int S_INT = 12345;
  public static String S_STRING = "string";
  public static long S_LONG = 0x1000200030004000L;
  public static double S_DOUBLE = 1.0000000000000002;
  public static float S_FLOAT = -2.0f;
}

class UnEncodable {
  public static final int S_INT = Math.random() > .5 ? 1 : 0;
}

class HasCharSequence {
  // CharSequence must not be processed because DalvikVM can't handle it.
  public static CharSequence S_CHARSEQ = "SEQ";
}

class FinalInlineV2ClinitReadAndWriteTest {
  private static int sCount = 0;
  private static final int TYPE_ZERO = sCount++;
  private static final int TYPE_ONE  = sCount++;
  private static final int TYPE_TWO  = sCount++;

  private static final String[] TYPES_TO_STR = new String[sCount];

  public static int getLength() {
    return TYPES_TO_STR.length;
  }
}
