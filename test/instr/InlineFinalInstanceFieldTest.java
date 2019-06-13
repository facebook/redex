/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;
import org.junit.Test;

class MixedTypeInstance {
  public final int m_final_accessed;
  public int m_non_final_accessed;
  public final int m_final_inlineable;
  public int m_non_final_inlineable;
  public int m_changed_0;
  public int m_changed_2;
  public int m_changed_4;
  public int m_changed_5;
  public int m_not_deletable;
  public int m_deletable;
  public MixedTypeInstance() {
    change0();
    m_final_accessed = 2;
    change2();
    m_final_inlineable = m_final_accessed;
    m_non_final_accessed = 4;
    change4();
    m_non_final_accessed = 5;
    change5();
    m_non_final_inlineable = m_non_final_accessed;
    m_non_final_accessed = 6;
    m_deletable = 0;
    m_not_deletable = 2;
    m_not_deletable = 0;
  }
  public void change0() {
    m_changed_0 = m_final_accessed;
  }
  public void  change2() {
    m_changed_2 = m_final_accessed;
  }
  public void  change4() {
    m_changed_4 = m_non_final_accessed;
  }
  public void  change5() {
    m_changed_5 = m_non_final_accessed;
  }
  public int return_final_inlineable() {
    // Should return 2
    return m_final_inlineable;
  }
  public int return_non_final_inlineable() {
    // Should return 5
    return m_non_final_inlineable;
  }
}

class EncodableFinal {
  public final boolean m_bool = true;
  public final byte m_byte = 'b';
  public final char m_char = 'c';
  public final short m_short = 128;
  public final int m_int = 12345;
  public final String m_string = "string";
  public final long m_long = 0x1000200030004000L;
  public final double m_double = 1.0000000000000002;
}

class NotFinal {
  public boolean m_bool = true;
  public byte m_byte = 'b';
  public char m_char = 'c';
  public short m_short = 128;
  public int m_int = 12345;
  public String m_string = "string";
  public long m_long = 0x1000200030004000L;
  public double m_double = 1.0000000000000002;
  public void changeMInt() {
    m_int = 12346;
  }
}

class UnEncodableFinal {
  public final int m_int = Math.random() > .5 ? 1 : 0;
}

class HasCharSequenceFinal {
  // CharSequence must not be processed because DalvikVM can't handle it.
  public final CharSequence m_charseq = "SEQ";
}

class OneInitCanReplaceFinal {
  public final int m_int;
  public OneInitCanReplaceFinal() {
    m_int = 1;
  }
}

class OneInitCantReplaceFinal {
  public final int m_int;
  public OneInitCantReplaceFinal(int a) {
    m_int = a;
  }
}

class TwoInitCantReplaceFinal {
  public final int m_int;
  public TwoInitCantReplaceFinal() {
    m_int = 5;
  }
  public TwoInitCantReplaceFinal(int a) {
    m_int = 6;
  }
}

class ReadInCtors1 {
  public final int m_int;
  public final int m_int_2;
  public ReadInCtors1() {
    m_int = 5;
    m_int_2 = m_int;
  }
}

class ReadInCtors2 {
  public final int m_int_3;
  public ReadInCtors2() {
    ReadInCtors1 a = new ReadInCtors1();
    m_int_3 = a.m_int;
  }
}

public class InlineFinalInstanceFieldTest {
  @Test
  public void testEncodableFinal() {
    EncodableFinal a = new EncodableFinal();
    assertThat(a.m_bool).isEqualTo(true);
    assertThat(a.m_byte).isEqualTo((byte) 'b');
    assertThat(a.m_char).isEqualTo('c');
    assertThat(a.m_short).isEqualTo((short) 128);
    assertThat(a.m_int).isEqualTo(12345);
    assertThat(a.m_string).isEqualTo("string");
    assertThat(a.m_long).isEqualTo(0x1000200030004000L);
    assertThat(a.m_double).isEqualTo(1.0000000000000002);
  }

  @Test
  public void testNotFinal() {
    NotFinal a = new NotFinal();
    assertThat(a.m_bool).isEqualTo(true);
    assertThat(a.m_byte).isEqualTo((byte) 'b');
    assertThat(a.m_char).isEqualTo('c');
    assertThat(a.m_short).isEqualTo((short) 128);
    assertThat(a.m_int).isEqualTo(12345);
    assertThat(a.m_string).isEqualTo("string");
    assertThat(a.m_long).isEqualTo(0x1000200030004000L);
    assertThat(a.m_double).isEqualTo(1.0000000000000002);
    a.m_string = "still a string";
    a.changeMInt();
    assertThat(a.m_string).isEqualTo("still a string");
    assertThat(a.m_int).isEqualTo(12346);
  }

  @Test
  public void testUnEncodableFinal() {
    UnEncodableFinal a = new UnEncodableFinal();
    assertThat(a.m_int == 0 || a.m_int == 1).isTrue();
  }

  @Test
  public void testHasCharSequenceFinal() {
    HasCharSequenceFinal a = new HasCharSequenceFinal();
    assertThat(a.m_charseq).isEqualTo("SEQ");
  }

  @Test
  public void testOneInitCanReplaceFinal() {
    OneInitCanReplaceFinal a = new OneInitCanReplaceFinal();
    assertThat(a.m_int).isEqualTo(1);
  }

  @Test
  public void testOneInitCantReplaceFinal() {
    OneInitCantReplaceFinal a = new OneInitCantReplaceFinal(2);
    assertThat(a.m_int).isEqualTo(2);
  }

  @Test
  public void testTwoInitCantReplaceFinal() {
    TwoInitCantReplaceFinal a = new TwoInitCantReplaceFinal();
    assertThat(a.m_int).isEqualTo(5);
  }

  @Test
  public void testMixedTypeInstance() {
    MixedTypeInstance a = new MixedTypeInstance();
    assertThat(a.return_final_inlineable()).isEqualTo(2);
    assertThat(a.m_final_inlineable).isEqualTo(2);
    assertThat(a.return_non_final_inlineable()).isEqualTo(5);
    assertThat(a.m_non_final_inlineable).isEqualTo(5);
    assertThat(a.m_final_accessed).isEqualTo(2);
    assertThat(a.m_non_final_accessed).isEqualTo(6);
    assertThat(a.m_changed_0).isEqualTo(0);
    assertThat(a.m_changed_2).isEqualTo(2);
    assertThat(a.m_changed_4).isEqualTo(4);
    assertThat(a.m_changed_5).isEqualTo(5);
    assertThat(a.m_deletable).isEqualTo(0);
    assertThat(a.m_not_deletable).isEqualTo(0);
  }

  @Test
  public void testReadInCtors() {
    ReadInCtors1 a = new ReadInCtors1();
    ReadInCtors2 b = new ReadInCtors2();
    assertThat(a.m_int).isEqualTo(5);
    assertThat(a.m_int_2).isEqualTo(5);
    assertThat(b.m_int_3).isEqualTo(5);
  }
}
