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

enum BigEnum {
  BIG1,
  BIG2,
  BIG3,
  BIG4,
  BIG5,
  BIG6,
  BIG7,
  BIG8,
  BIG9,
  BIG10,
  BIG11,
  BIG12,
  BIG13,
  BIG14,
  BIG15,
  BIG16,
  BIG17,
  BIG18,
  BIG19,
  BIG20;
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

  int with_other_code(EnumB element, boolean b, Object o) {
    try {
      if (b) {
        switch (element) {
        case TYPE_B_0:
          return 0;
        case TYPE_B_1:
          return 10;
        case TYPE_B_2:
          return 20;
        default:
          return 500;
        }
      } else {
        return o.hashCode();
      }
    } catch (NullPointerException npe) {
      return 404;
    }
  }

  int useBigEnum(BigEnum element) {
    switch (element) {
    case BIG1:
      return 1;
    case BIG2:
      return 2;
    case BIG3:
      return 3;
    case BIG4:
      return 4;
    case BIG5:
      return 5;
    case BIG6:
      return 6;
    case BIG7:
      return 7;
    case BIG8:
      return 8;
    case BIG9:
      return 9;
    case BIG10:
      return 10;
    case BIG11:
      return 11;
    case BIG12:
      return 12;
    case BIG13:
      return 13;
    case BIG14:
      return 14;
    case BIG15:
      return 15;
    case BIG16:
      return 16;
    case BIG17:
      return 17;
    case BIG18:
      return 18;
    case BIG19:
      return 19;
    case BIG20:
      return 20;
    default:
      throw new IllegalArgumentException();
    }
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

    Object o = new Object();
    assertThat(foo.with_other_code(EnumB.TYPE_B_0, true, null)).isEqualTo(0);
    assertThat(foo.with_other_code(EnumB.TYPE_B_1, true, null)).isEqualTo(10);
    assertThat(foo.with_other_code(EnumB.TYPE_B_2, true, o)).isEqualTo(20);
    assertThat(foo.with_other_code(EnumB.TYPE_B_0, false, null)).isEqualTo(404);
    assertThat(foo.with_other_code(EnumB.TYPE_B_1, false, o))
        .isEqualTo(o.hashCode());
    assertThat(foo.with_other_code(EnumB.TYPE_B_2, false, null)).isEqualTo(404);
  }

  @Test
  public void testBigEnum() {
    Foo foo = new Foo();
    assertThat(foo.useBigEnum(BigEnum.BIG1)).isEqualTo(1);
    assertThat(foo.useBigEnum(BigEnum.BIG2)).isEqualTo(2);
    assertThat(foo.useBigEnum(BigEnum.BIG3)).isEqualTo(3);
    assertThat(foo.useBigEnum(BigEnum.BIG4)).isEqualTo(4);
    assertThat(foo.useBigEnum(BigEnum.BIG5)).isEqualTo(5);
    assertThat(foo.useBigEnum(BigEnum.BIG6)).isEqualTo(6);
    assertThat(foo.useBigEnum(BigEnum.BIG7)).isEqualTo(7);
    assertThat(foo.useBigEnum(BigEnum.BIG8)).isEqualTo(8);
    assertThat(foo.useBigEnum(BigEnum.BIG9)).isEqualTo(9);
    assertThat(foo.useBigEnum(BigEnum.BIG10)).isEqualTo(10);
    assertThat(foo.useBigEnum(BigEnum.BIG11)).isEqualTo(11);
    assertThat(foo.useBigEnum(BigEnum.BIG12)).isEqualTo(12);
    assertThat(foo.useBigEnum(BigEnum.BIG13)).isEqualTo(13);
    assertThat(foo.useBigEnum(BigEnum.BIG14)).isEqualTo(14);
    assertThat(foo.useBigEnum(BigEnum.BIG15)).isEqualTo(15);
    assertThat(foo.useBigEnum(BigEnum.BIG16)).isEqualTo(16);
    assertThat(foo.useBigEnum(BigEnum.BIG17)).isEqualTo(17);
    assertThat(foo.useBigEnum(BigEnum.BIG18)).isEqualTo(18);
    assertThat(foo.useBigEnum(BigEnum.BIG19)).isEqualTo(19);
    assertThat(foo.useBigEnum(BigEnum.BIG20)).isEqualTo(20);
  }
}
