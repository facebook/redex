/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.*;

import org.junit.Before;
import org.junit.Test;

public class ConstantPropagationTest {

  // use MethodInline so redex sees these constants but `javac` and d8 do not
  long getLong() {
    return 0x0002000300040005L;
  }

  @Test
  public void if_long() {
    long x = getLong();
    int y;
    if (x > 0x0001000200030004L) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  long getNegLong() {
    return 0x0000000fffffffffL;
  }

  @Test
  public void if_negative_long() {
    long x = getNegLong();
    int y;
    if (x == -1L) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(0);
  }

  float getFloat1() {
    return 3.0f;
  }

  @Test
  public void if_float1() {
    float x = getFloat1();
    int y;
    if (x <= 4.5f) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  float getFloat2() {
    return 2.25f;
  }

  @Test
  public void if_float2() {
    float x = getFloat2();
    int y;
    if (x == 2.25f) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  double getDouble1() {
    return 3.0;
  }

  @Test
  public void if_double1() {
    double x = getDouble1();
    int y;
    if (x <= 4.5) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  double getDouble2() {
    return 2.26;
  }

  @Test
  public void if_double2() {
    double x = getDouble2();
    int y;
    if (x >= 2.27) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(0);
  }

  private int neg1() {
    return -1;
  }

  @Test
  public void if_plus_one() {
    int x = neg1();
    x++;
    int y;
    if (x == 0) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  private int one() {
    return 1;
  }

  // This test intentionally does not use the same constant literal more than
  // once. The reason is that d8 will load that constant and re-use the
  // register rather than using a add-int/lit instruction. Currently
  // ConstantPropagationPass only propagates add-int/lit but not add-int.
  // TODO: propagate more than just add-int/lit
  @Test
  public void if_plus_two() {
    int x = one();
    x += 2;
    int y;
    if (x == 3) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  @Test
  public void overflow() {
    int x = Integer.MAX_VALUE;
    x++;
    assertThat(x).isEqualTo(Integer.MIN_VALUE);
  }

  @Test
  public void lit_minus() {
    int x = one();
    int y = 5 - x;
    int z;
    if (y == 4) {
      z = 42;
    } else {
      z = 0;
    }
    assertThat(z).isEqualTo(42);
  }
}
