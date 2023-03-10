/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import org.junit.Test;

import static org.assertj.core.api.Assertions.assertThat;

abstract class Base {}
class S1 extends Base {
  int getFoo() { return 42; }
}
class S2 extends Base {
  int getFoo() { return 43; }
}
class S3 extends Base {
  int getFoo() { return 44; }
}
class S4 extends Base {
  int getFoo() { return 45; }
}
class S5 extends Base {
  int getFoo() { return 46; }
}
class S6 extends Base {
  int getFoo() { return 47; }
}

abstract class SecondBase {}
class Q1 extends SecondBase {
  int getFoo() { return 42; }
}
class Q2 extends SecondBase {
  int getFoo() { return 43; }
}
class Q3 extends SecondBase {
  int getFoo() { return 44; }
}
class Q4 extends SecondBase {
  int getFoo() { return 45; }
}
class Q5 extends SecondBase {
  int getFoo() { return 46; }
}
class Q6 extends SecondBase {
  int getFoo() { return 47; }
}
class Q7 extends SecondBase {
  int getFoo() { return 48; }
}

public class ClassMergingGroupingTest {

  @Test
  public void testMergingBase() {
    S1 s1 = new S1();
    S2 s2 = new S2();
    S3 s3 = new S3();
    S4 s4 = new S4();
    S5 s5 = new S5();
    S6 s6 = new S6();
    assertThat(s1.getFoo()).isEqualTo(42);
    assertThat(s2.getFoo()).isEqualTo(43);
    assertThat(s3.getFoo()).isEqualTo(44);
    assertThat(s4.getFoo()).isEqualTo(45);
    assertThat(s5.getFoo()).isEqualTo(46);
    assertThat(s6.getFoo()).isEqualTo(47);
  }

  @Test
  public void testMergingSecondBase() {
    Q1 q1 = new Q1();
    Q2 q2 = new Q2();
    Q3 q3 = new Q3();
    Q4 q4 = new Q4();
    Q5 q5 = new Q5();
    Q6 q6 = new Q6();
    Q7 q7 = new Q7();
    assertThat(q1.getFoo()).isEqualTo(42);
    assertThat(q2.getFoo()).isEqualTo(43);
    assertThat(q3.getFoo()).isEqualTo(44);
    assertThat(q4.getFoo()).isEqualTo(45);
    assertThat(q5.getFoo()).isEqualTo(46);
    assertThat(q6.getFoo()).isEqualTo(47);
    assertThat(q7.getFoo()).isEqualTo(48);
  }
}
