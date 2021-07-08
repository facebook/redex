/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class FooDelagates {
  val lazyValue: String by lazy { expensiveFn() }
  fun expensiveFn(): String {
    return "Help"
  }
}

class DelegateTest {
  fun main() {
    val foo = FooDelagates()
    println(foo.lazyValue)
    println(foo.lazyValue)
  }
}
