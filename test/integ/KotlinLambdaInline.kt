/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class KotlinLambdaInline {
  var sink: Long = 0

  fun doCalc(addfn: (a: Long, b: Long) -> Long): Long {
    return addfn(123L, 456L)
  }

  fun foo() {
    sink = doCalc { a: Long, b: Long -> a + b }
  }

  fun bar() {
    // Same lambda body as foo - will be a duplicate trivial lambda
    sink = doCalc { a: Long, b: Long -> a + b }
  }

  fun baz() {
    // Different lambda body - will be a unique trivial lambda
    sink = doCalc { a: Long, b: Long -> a - b }
  }
}
