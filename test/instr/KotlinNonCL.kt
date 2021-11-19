/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Use this example to test kotlin non-capturing lambda inling.
class KtNonCapturingLambda {
  var sink: Long = 0
  var ret: Boolean = false

  fun doCalc(addfn: (a: Long, b: Long) -> Long): Long {
    return addfn(123L, 456L)
  }

  fun doCalc1(orfn: (a: Boolean, b: Boolean) -> Boolean): Boolean {
    return orfn(true, false)
  }

  fun foo() {
    sink = doCalc { a: Long, b: Long -> a + b }
  }

  fun foo1() {
    ret = doCalc1 { a: Boolean, b: Boolean -> a && b }
  }

  fun main() {
    foo()
    foo1()
  }
}
