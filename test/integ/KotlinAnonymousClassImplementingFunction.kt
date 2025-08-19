/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import kotlin.jvm.functions.Function2

// Sanity check, an anonymous class implementing Kotlin functions don't have a
// singleton.
class KotlinAnonymousClassImplementingFunction {
  var sink: Long = 0

  fun doCalc(addfn: (a: Long, b: Long) -> Long): Long {
    return addfn(123L, 456L)
  }

  fun foo() {
    val addfn =
        object : Function2<Int, Int, Int> {
          override fun invoke(a: Int, b: Int) = a + b
        }
    sink = doCalc { a: Long, b: Long -> (a + b) }
  }
}
