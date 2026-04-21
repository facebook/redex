/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

object AreEqualWrappers {
  // CHECK: method: direct redex.AreEqualWrappers.firstFiltered:
  @JvmStatic
  fun firstFiltered(a: Any?, b: Any?): Boolean {
    if (a == null) error("a must be non-null")
    // Now a is known to be non-null.
    // PRECHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.areEqual:
    // POSTCHECK-NOT: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.areEqual:
    // POSTCHECK: invoke-virtual {{.*}} java.lang.Object.equals:
    return a == b
  }

  // CHECK: method: direct redex.AreEqualWrappers.secondFiltered:
  @JvmStatic
  fun secondFiltered(a: Class<*>?, b: Class<*>?): Boolean {
    if (b == null) error("b must be non-null")
    // Now b is known to be non-null.
    // PRECHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.areEqual:
    // POSTCHECK-NOT: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.areEqual:
    // POSTCHECK: invoke-virtual {{.*}} java.lang.Object.equals:
    return a == b
  }

  // CHECK: method: direct redex.AreEqualWrappers.secondFiltered:
  @JvmStatic
  fun secondFiltered(a: Any?, b: Any?): Boolean {
    if (b == null) error("b must be non-null")
    // Now b is known to be non-null, but Any (Object) is not in the
    // safe-symmetric-equals set, so neither swap nor replacement fires.
    // CHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.areEqual:
    return a == b
  }

  // CHECK: method: direct redex.AreEqualWrappers.secondFiltered:
  @JvmStatic
  fun secondFiltered(a: String?, b: String?): Boolean {
    if (b == null) error("b must be non-null")
    // Now b is known to be non-null.
    // PRECHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.areEqual:
    // POSTCHECK-NOT: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.areEqual:
    // POSTCHECK: invoke-virtual {{.*}} java.lang.Object.equals:
    return a == b
  }

  // CHECK: method: direct redex.AreEqualWrappers.unfiltered:
  @JvmStatic
  fun unfiltered(a: Any?, b: Any?): Boolean {
    // CHECK: invoke-static {{.*}} kotlin.jvm.internal.Intrinsics.areEqual:
    return a == b
  }
}

// Each test method below pins its `invoke-static AreEqualWrappers.*` call
// sites with CHECK directives to sanity-check that constant folding doesn't
// happen for our tests.
class ReplaceKotlinAreEqualTest {
  // CHECK: method: virtual redex.ReplaceKotlinAreEqualTest.bothNonNull_equalContent_returnsTrue:
  @Test
  fun bothNonNull_equalContent_returnsTrue() {
    val s1 = "hello"
    val s2 = "hello"
    val c1: Class<*> = String::class.java
    val c2: Class<*> = String::class.java
    val o1: Any = 42
    val o2: Any = 42
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.unfiltered:
    assertTrue(AreEqualWrappers.unfiltered(s1, s2))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.firstFiltered:
    assertTrue(AreEqualWrappers.firstFiltered(s1, s2))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.secondFiltered:
    assertTrue(AreEqualWrappers.secondFiltered(s1, s2))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.secondFiltered:
    assertTrue(AreEqualWrappers.secondFiltered(c1, c2))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.secondFiltered:
    assertTrue(AreEqualWrappers.secondFiltered(o1, o2))
  }

  // CHECK: method: virtual redex.ReplaceKotlinAreEqualTest.bothNonNull_unequalContent_returnsFalse:
  @Test
  fun bothNonNull_unequalContent_returnsFalse() {
    val s1 = "hello"
    val s2 = "world"
    val c1: Class<*> = String::class.java
    val c2: Class<*> = Integer::class.java
    val o1: Any = 42
    val o2: Any = 99
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.unfiltered:
    assertFalse(AreEqualWrappers.unfiltered(s1, s2))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.firstFiltered:
    assertFalse(AreEqualWrappers.firstFiltered(s1, s2))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.secondFiltered:
    assertFalse(AreEqualWrappers.secondFiltered(s1, s2))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.secondFiltered:
    assertFalse(AreEqualWrappers.secondFiltered(c1, c2))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.secondFiltered:
    assertFalse(AreEqualWrappers.secondFiltered(o1, o2))
  }

  // CHECK: method: virtual redex.ReplaceKotlinAreEqualTest.bothNull_returnsTrue:
  @Test
  fun bothNull_returnsTrue() {
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.unfiltered:
    assertTrue(AreEqualWrappers.unfiltered(null, null))
  }

  // CHECK: method: virtual redex.ReplaceKotlinAreEqualTest.firstNonNullSecondNull_returnsFalse:
  @Test
  fun firstNonNullSecondNull_returnsFalse() {
    val a = "hello"
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.unfiltered:
    assertFalse(AreEqualWrappers.unfiltered(a, null))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.firstFiltered:
    assertFalse(AreEqualWrappers.firstFiltered(a, null))
  }

  // CHECK: method: virtual redex.ReplaceKotlinAreEqualTest.firstNullSecondNonNull_returnsFalse:
  @Test
  fun firstNullSecondNonNull_returnsFalse() {
    val s = "hello"
    val c: Class<*> = String::class.java
    val o: Any = 42
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.unfiltered:
    assertFalse(AreEqualWrappers.unfiltered(null, s))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.secondFiltered:
    assertFalse(AreEqualWrappers.secondFiltered(null, s))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.secondFiltered:
    assertFalse(AreEqualWrappers.secondFiltered(null, c))
    // CHECK: invoke-static {{.*}} redex.AreEqualWrappers.secondFiltered:
    assertFalse(AreEqualWrappers.secondFiltered(null, o))
  }
}
