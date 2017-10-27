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

import org.junit.Before;
import org.junit.Test;

public class ConstantPropagationTest {

  @Test
  public void if_long() {
    long x = 0x0002000300040005L;
    int y;
    if (x > 0x0001000200030004L) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  @Test
  public void if_negative_long() {
    long x = 0x0000000fffffffffL;
    int y;
    if (x == -1L) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(0);
  }

  @Test
  public void if_float1() {
    float x = 3.0f;
    int y;
    if (x <= 4.5f) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  @Test
  public void if_float2() {
    float x = 2.25f;
    int y;
    if (x == 2.25f) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  @Test
  public void if_double1() {
    double x = 3.0;
    int y;
    if (x <= 4.5) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  @Test
  public void if_double2() {
    double x = 2.26;
    int y;
    if (x >= 2.27) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(0);
  }

  // use SimpleInline so that redex sees this constant but `javac` does not
  private int neg1() {
    return -1;
  }

  // use SimpleInline so that redex sees this constant but `javac` does not
  private int zero() {
    return 0;
  }

  @Test
  public void if_plus_one() {
    int x = neg1();
    x++;
    assertThat(x).isEqualTo(0);
    int y;
    if (x == 0) {
      y = 32;
    } else {
      y = 0;
    }
    assertThat(y).isEqualTo(32);
  }

  @Test
  public void if_plus_one2() {
    int x = zero();
    x++;
    assertThat(x).isEqualTo(1);
    int y;
    if (x == 1) {
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
}
