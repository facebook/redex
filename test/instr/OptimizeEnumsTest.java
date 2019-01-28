/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import org.junit.Test;
import static org.fest.assertions.api.Assertions.assertThat;

enum EnumA {
  TYPE_A_0,
  TYPE_A_1,
  TYPE_A_2;
}

enum EnumB {
  TYPE_B_0,
  TYPE_B_1,
  TYPE_B_2;
}

class Foo {
  int useEnumA(EnumA element) {
    switch (element) {
      case TYPE_A_0:
        return 0;

      case TYPE_A_2:
        return 2;
    }

    return 1;
  }

  int useEnumB(EnumB element) {
    switch (element) {
      case TYPE_B_0:
        return 3;

      case TYPE_B_2:
        return 5;
    }

    return 4;
  }

  int useEnumA_again(EnumA element) {
    switch (element) {
      case TYPE_A_0:
        return 6;

      case TYPE_A_1:
        return 7;
    }

    return 8;
  }
}

public class OptimizeEnumsTest {
  @Test
  public void testEnumA() {
    Foo foo = new Foo();
    assertThat(foo.useEnumA(EnumA.TYPE_A_0)).isEqualTo(0);
    assertThat(foo.useEnumA(EnumA.TYPE_A_1)).isEqualTo(1);
    assertThat(foo.useEnumA(EnumA.TYPE_A_2)).isEqualTo(2);

    assertThat(foo.useEnumA_again(EnumA.TYPE_A_0)).isEqualTo(6);
    assertThat(foo.useEnumA_again(EnumA.TYPE_A_1)).isEqualTo(7);
    assertThat(foo.useEnumA_again(EnumA.TYPE_A_2)).isEqualTo(8);
  }

  @Test
  public void testEnumB() {
    Foo foo = new Foo();
    assertThat(foo.useEnumB(EnumB.TYPE_B_0)).isEqualTo(3);
    assertThat(foo.useEnumB(EnumB.TYPE_B_1)).isEqualTo(4);
    assertThat(foo.useEnumB(EnumB.TYPE_B_2)).isEqualTo(5);
  }
}
