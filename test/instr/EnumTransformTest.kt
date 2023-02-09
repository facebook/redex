/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex

import org.fest.assertions.api.Assertions.assertThat
import org.junit.Test

// The following rules are about the helper class synthesized by OptimizeEnumsPass.
// POSTCHECK-LABEL: class: redex.$EnumUtils
// POSTCHECK-NEXT: Access flags: (PUBLIC, FINAL)
// POSTCHECK-NEXT: Superclass: java.lang.Object
// POSTCHECK: (PRIVATE, STATIC, FINAL) $VALUES:java.lang.Integer[]
// POSTCHECK: (PUBLIC, STATIC, FINAL) f0:java.lang.Integer
// POSTCHECK: (PUBLIC, STATIC, FINAL) f1:java.lang.Integer
// POSTCHECK: (PUBLIC, STATIC, FINAL) f2:java.lang.Integer
// POSTCHECK: (PUBLIC, STATIC, FINAL) f3:java.lang.Integer
// POSTCHECK-NOT: (PUBLIC, STATIC, FINAL) f4:java.lang.Integer

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

// Not optimized due to the upcast to Any (Object in Java).
// Note that the is type check in useEnumH is not what rejects the optimization,
// but the upcast preceding that is the reason.
// CHECK-LABEL: class: redex.H
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Enum
enum class H {
  H0,
  H1,
  H2
}

// Not optimized due to the upcast to java.io.Serializable.
// Note that on the return value, the as type cast could be implicit.
// CHECK-LABEL: class: redex.J
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Enum
enum class J {
  J0,
  J1,
  J2
}

// Not optimized if referenced by reflection.
// CHECK-LABEL: class: redex.K
// CHECK-NEXT: Access flags:
// PRECHECK-NEXT: Superclass: java.lang.Enum
// POSTCHECK-NEXT: Superclass: java.lang.Enum
enum class K {
  K0,
  K1,
  K2
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

    fun useEnumAValueOf(s: String): Int {
      val elem: A = A.valueOf(s)
      return when (elem) {
        A.A0 -> 40
        A.A2 -> 42
        else -> 41
      }
    }

    fun useEnumAValues(): Int {
      val vals = A.values()
      return vals.size
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

    fun useEnumH(elem: Any): String {
      if (elem is H) {
        return elem.name
      } else {
        return "Not H"
      }
    }

    fun useEnumJ(elem: J): java.io.Serializable {
      return elem as java.io.Serializable
    }

    fun useEnumK(elem: K): String = elem::class.java.simpleName
  }
}

class EnumTransformTest {

  @Test
  fun testA() {
    assertThat(Helper.Companion.useEnumA(A.A0)).isEqualTo(40)
    assertThat(Helper.Companion.useEnumA(A.A1)).isEqualTo(41)
    assertThat(Helper.Companion.useEnumA(A.A2)).isEqualTo(42)

    assertThat(Helper.Companion.useEnumAValueOf("A0")).isEqualTo(40)
    assertThat(Helper.Companion.useEnumAValueOf("A1")).isEqualTo(41)
    assertThat(Helper.Companion.useEnumAValueOf("A2")).isEqualTo(42)

    assertThat(Helper.Companion.useEnumAValues()).isEqualTo(3)
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

  @Test
  fun testH() {
    assertThat(Helper.Companion.useEnumH(H.H0)).isEqualTo("H0")
    assertThat(Helper.Companion.useEnumH(H.H1)).isEqualTo("H1")
    assertThat(Helper.Companion.useEnumH(H.H2)).isEqualTo("H2")
    assertThat(Helper.Companion.useEnumH(1)).isEqualTo("Not H")
  }

  @Test
  fun testJ() {
    assertThat(Helper.Companion.useEnumJ(J.J0)).isNotNull()
    assertThat(Helper.Companion.useEnumJ(J.J1)).isNotNull()
    assertThat(Helper.Companion.useEnumJ(J.J2)).isNotNull()
  }

  @Test
  fun testK() {
    assertThat(Helper.Companion.useEnumK(K.K0)).isEqualTo("K")
    assertThat(Helper.Companion.useEnumK(K.K1)).isEqualTo("K")
  }
}
