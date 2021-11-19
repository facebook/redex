/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest.objtest
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
    @JvmStatic var someOtherStr: String = "Bar"
    @JvmStatic
    fun funX(): String {
      return someOtherStr
    }
  }
}

class Foo {
  fun main() {
    val obj = CompanionClass()
    print(obj.get())
    print(CompanionClass.someStr)
    print(AnotherCompanionClass.someOtherStr)
    print(AnotherCompanionClass.funX())
  }
}
