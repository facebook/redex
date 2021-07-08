/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
}
