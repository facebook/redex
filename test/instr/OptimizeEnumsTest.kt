/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest.kt

import org.fest.assertions.api.Assertions.assertThat
import org.junit.Test

enum class A {
  A0,
  A1,
  A2
}

enum class B {
  B0,
  B1,
  B2
}

enum class Big {
  BIG01,
  BIG02,
  BIG03,
  BIG04,
  BIG05,
  BIG06,
  BIG07,
  BIG08,
  BIG09,
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
  BIG20
}

public fun useA(elem: A): Int =
    when (elem) {
      A.A0 -> 40
      A.A2 -> 42
      else -> 41
    }

public fun useB(elem: B): Int =
    when (elem) {
      B.B0 -> 43
      B.B2 -> 45
      else -> 44
    }

public fun useAAgain(elem: A): Int =
    when (elem) {
      A.A0 -> 46
      A.A1 -> 47
      else -> 48
    }

public fun withOtherCode(elem: B, b: Boolean, o: Any?): Int =
    try {
      if (b) {
        when (elem) {
          B.B0 -> 49
          B.B1 -> 50
          B.B2 -> 51
          else -> 500
        }
      } else {
        o!!.hashCode()
      }
    } catch (_: NullPointerException) {
      404
    }

public fun useBig(elem: Big): Int =
    when (elem) {
      Big.BIG01 -> 1
      Big.BIG02 -> 2
      Big.BIG03 -> 3
      Big.BIG04 -> 4
      Big.BIG05 -> 5
      Big.BIG06 -> 6
      Big.BIG07 -> 7
      Big.BIG08 -> 8
      Big.BIG09 -> 9
      Big.BIG10 -> 10
      Big.BIG11 -> 11
      Big.BIG12 -> 12
      Big.BIG13 -> 13
      Big.BIG14 -> 14
      Big.BIG15 -> 15
      Big.BIG16 -> 16
      Big.BIG17 -> 17
      Big.BIG18 -> 18
      Big.BIG19 -> 19
      Big.BIG20 -> 20
      else -> throw IllegalArgumentException()
    }

public fun useBigNullable(elem: Big?): Int =
    when (elem) {
      Big.BIG01 -> 1
      Big.BIG02 -> 2
      Big.BIG03 -> 3
      Big.BIG04 -> 4
      Big.BIG05 -> 5
      Big.BIG06 -> 6
      Big.BIG07 -> 7
      Big.BIG08 -> 8
      Big.BIG09 -> 9
      Big.BIG10 -> 10
      Big.BIG11 -> 11
      Big.BIG12 -> 12
      Big.BIG13 -> 13
      Big.BIG14 -> 14
      Big.BIG15 -> 15
      Big.BIG16 -> 16
      Big.BIG17 -> 17
      Big.BIG18 -> 18
      Big.BIG19 -> 19
      null -> 0
      else -> -1
    }

class OptimizeEnumsTest {
  @Test
  fun testA() {
    assertThat(useA(A.A0)).isEqualTo(40)
    assertThat(useA(A.A1)).isEqualTo(41)
    assertThat(useA(A.A2)).isEqualTo(42)

    assertThat(useAAgain(A.A0)).isEqualTo(46)
    assertThat(useAAgain(A.A1)).isEqualTo(47)
    assertThat(useAAgain(A.A2)).isEqualTo(48)
  }

  @Test
  fun testB() {
    assertThat(useB(B.B0)).isEqualTo(43)
    assertThat(useB(B.B1)).isEqualTo(44)
    assertThat(useB(B.B2)).isEqualTo(45)

    val o = Object()
    assertThat(withOtherCode(B.B0, true, null)).isEqualTo(49)
    assertThat(withOtherCode(B.B1, true, null)).isEqualTo(50)
    assertThat(withOtherCode(B.B2, true, o)).isEqualTo(51)
    assertThat(withOtherCode(B.B0, false, null)).isEqualTo(404)
    assertThat(withOtherCode(B.B1, false, o)).isEqualTo(o.hashCode())
    assertThat(withOtherCode(B.B2, false, null)).isEqualTo(404)
  }

  @Test
  fun testBig() {
    assertThat(useBig(Big.BIG01)).isEqualTo(1)
    assertThat(useBig(Big.BIG02)).isEqualTo(2)
    assertThat(useBig(Big.BIG03)).isEqualTo(3)
    assertThat(useBig(Big.BIG04)).isEqualTo(4)
    assertThat(useBig(Big.BIG05)).isEqualTo(5)
    assertThat(useBig(Big.BIG06)).isEqualTo(6)
    assertThat(useBig(Big.BIG07)).isEqualTo(7)
    assertThat(useBig(Big.BIG08)).isEqualTo(8)
    assertThat(useBig(Big.BIG09)).isEqualTo(9)
    assertThat(useBig(Big.BIG10)).isEqualTo(10)
    assertThat(useBig(Big.BIG11)).isEqualTo(11)
    assertThat(useBig(Big.BIG12)).isEqualTo(12)
    assertThat(useBig(Big.BIG13)).isEqualTo(13)
    assertThat(useBig(Big.BIG14)).isEqualTo(14)
    assertThat(useBig(Big.BIG15)).isEqualTo(15)
    assertThat(useBig(Big.BIG16)).isEqualTo(16)
    assertThat(useBig(Big.BIG17)).isEqualTo(17)
    assertThat(useBig(Big.BIG18)).isEqualTo(18)
    assertThat(useBig(Big.BIG19)).isEqualTo(19)
    assertThat(useBig(Big.BIG20)).isEqualTo(20)
  }
  @Test
  fun testBigNullable() {
    assertThat(useBigNullable(Big.BIG01)).isEqualTo(1)
    assertThat(useBigNullable(Big.BIG20)).isEqualTo(-1)
    assertThat(useBigNullable(null)).isEqualTo(0)
  }
}
