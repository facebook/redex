/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex;

import static org.fest.assertions.api.Assertions.assertThat;

import org.junit.Test;

class TestA {
  public int a;
  public int b;

  public TestA() {
    a = 0;
    b = 1;
  }

  public TestA(int param) { b = param; }
}

class TestB {
  public int a;
  public int b;

  public void change_ifield() {
    double random = Math.random();
    if (random > 1) {
      a = 0;
      b = 1;
    } else {
      a = 0;
      b = 0;
    }
  }
}

public class IPConstantPropagationTest {

  @Test
  public void two_ctors() {
    TestA one = new TestA();
    assertThat(one.a).isEqualTo(0);
    assertThat(one.b).isEqualTo(1);
    TestA two = new TestA(0);
    assertThat(two.a).isEqualTo(0);
    assertThat(two.b).isEqualTo(0);
  }

  @Test
  public void modified_elsewhere() {
    TestB one = new TestB();
    one.change_ifield();
    assertThat(one.a).isEqualTo(0);
    assertThat(one.b).isEqualTo(0);
  }
}
