/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Sanity check, stateful lambdas don't have singletons.
class KotlinStatefulLambda {
  var sink: Long = 0

  fun doCalc(addfn: (a: Long, b: Long) -> Long): Long {
    return addfn(123L, 456L)
  }

  fun foo() {
    val multiplier = 2
    sink = doCalc { a: Long, b: Long -> (a + b) * multiplier }
  }
}
