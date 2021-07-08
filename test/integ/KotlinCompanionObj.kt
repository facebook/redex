/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class CompanionClass {
  companion object {
    var someStr: String = "Foo"
  }
  fun get(): String {
    return someStr
  }
}

class AnotherCompanionClass {
  companion object Test {
    var someOtherStr: String = "Bar"
  }
}

fun main() {
  val obj = CompanionClass()
  print(obj.get())
  print(AnotherCompanionClass.someOtherStr)
}
