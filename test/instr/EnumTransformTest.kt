/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex

import org.fest.assertions.api.Assertions.assertThat
import org.junit.Test

// Simple case is optimized.
// CHECK-LABEL: class: redex.A
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Object
enum class A {
  A0,
  A1,
  A2
}

interface I {
  fun getVal(): Int
}

// Not optimized if implements an interface.
// CHECK-LABEL: class: redex.B
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Enum
enum class B : I {
  B0,
  B1,
  B2;
  override fun getVal(): Int {
    return 42
  }
}

// Enum w/ Int field is optimized.
// CHECK-LABEL: class: redex.C
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Object
enum class C(val idx: Int) {
  C0(1),
  C1(2),
  C2(3)
}

// Enum w/ String field is optimized.
// CHECK-LABEL: class: redex.D
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Object
enum class D(val bname: String) {
  D0("Tool"),
  D1("APC"),
  D2("Puscifer")
}

data class User(val name: String)

// Enum w/ field of custom type is not optimized.
// CHECK-LABEL: class: redex.E
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Enum
enum class E(val u: User) {
  E0(User("Hi")),
  E1(User("Yo")),
  E2(User("There"))
}

// Not optimized if enum constant is declared with anonymous class w/ overriding method.
// CHECK-LABEL: class: redex.ProtocolState
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Enum
enum class ProtocolState {
  WAITING {
    override fun signal(): ProtocolState = TALKING
  },
  TALKING {
    override fun signal(): ProtocolState = WAITING
  };

  abstract fun signal(): ProtocolState
}

// Enum w/ companion static method is optimized
// CHECK-LABEL: class: redex.F
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Object
enum class F {
  F0,
  F1,
  F2;
  companion object {
    fun getFByName(name: String): F = valueOf(name.toUpperCase())
  }
}

class Helper {
  companion object {
    // This method cannot be in the Test class to avoid Proguard keep rules.
    fun useEnumA(elem: A): Int =
        when (elem) {
          A.A0 -> 40
          A.A2 -> 42
          else -> 41
        }

    fun useEnumB(elem: B): Int =
        when (elem) {
          B.B1 -> 41
          B.B2 -> 42
          else -> 40
        }

    fun useEnumC(elem: C): Int =
        when (elem) {
          C.C0 -> 40
          C.C1 -> 41
          else -> 42
        }

    fun useEnumD(elem: D): String =
        when (elem) {
          D.D0 -> "Tool"
          D.D1 -> "APC"
          else -> "Puscifer"
        }

    fun useEnumE(elem: E): String =
        when (elem) {
          E.E0 -> "Hi"
          E.E1 -> "Yo"
          else -> "There"
        }

    fun useEnumProtocolState(elem: ProtocolState): String =
        when (elem.signal()) {
          ProtocolState.TALKING -> "waiting"
          else -> "talking"
        }

    fun useEnumF(name: String): String =
        when (F.Companion.getFByName(name)) {
          F.F0 -> "F0"
          F.F1 -> "F1"
          else -> "F2"
        }
  }
}

class EnumTransformTest {

  @Test
  fun testA() {
    assertThat(Helper.Companion.useEnumA(A.A0)).isEqualTo(40)
    assertThat(Helper.Companion.useEnumA(A.A1)).isEqualTo(41)
    assertThat(Helper.Companion.useEnumA(A.A2)).isEqualTo(42)
  }

  @Test
  fun testB() {
    assertThat(Helper.Companion.useEnumB(B.B0)).isEqualTo(40)
    assertThat(Helper.Companion.useEnumB(B.B1)).isEqualTo(41)
    assertThat(Helper.Companion.useEnumB(B.B2)).isEqualTo(42)
  }

  @Test
  fun testC() {
    assertThat(Helper.Companion.useEnumC(C.C0)).isEqualTo(40)
    assertThat(Helper.Companion.useEnumC(C.C1)).isEqualTo(41)
    assertThat(Helper.Companion.useEnumC(C.C2)).isEqualTo(42)
  }

  @Test
  fun testD() {
    assertThat(Helper.Companion.useEnumD(D.D0)).isEqualTo("Tool")
    assertThat(Helper.Companion.useEnumD(D.D1)).isEqualTo("APC")
    assertThat(Helper.Companion.useEnumD(D.D2)).isEqualTo("Puscifer")
  }

  @Test
  fun testE() {
    assertThat(Helper.Companion.useEnumE(E.E0)).isEqualTo("Hi")
    assertThat(Helper.Companion.useEnumE(E.E1)).isEqualTo("Yo")
    assertThat(Helper.Companion.useEnumE(E.E2)).isEqualTo("There")
  }

  @Test
  fun testProtocolState() {
    assertThat(Helper.Companion.useEnumProtocolState(ProtocolState.TALKING)).isEqualTo("talking")
  }

  @Test
  fun testF() {
    assertThat(Helper.Companion.useEnumF("f0")).isEqualTo("F0")
    assertThat(Helper.Companion.useEnumF("f1")).isEqualTo("F1")
    assertThat(Helper.Companion.useEnumF("f2")).isEqualTo("F2")
  }
}
