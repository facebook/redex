/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest

import kotlin.sequences.sequence
import kotlinx.coroutines.CoroutineName
import kotlinx.coroutines.CoroutineScope

internal abstract class Base() {
  open abstract fun fibonacciSeq(x: Int): Sequence<Int>
}

internal class A : Base() {
  override fun fibonacciSeq(x: Int): Sequence<Int> = sequence {
    var b = 1
    var a = x

    yield(5)

    while (true) {
      yield(a + b)

      val tmp = a + b
      a = b
      b = tmp
    }
  }
}

class ClassMergingKotlinCoroutinesTest {
  fun main(args: Array<String>) {
    val a = A()

    a.fibonacciSeq(0).take(5).toList()

    CoroutineScope(CoroutineName("Parent"))
  }
}
