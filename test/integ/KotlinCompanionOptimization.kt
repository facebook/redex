/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest.objtest

// Basic companion object: the pass relocates getSomeStr (the auto-generated
// property accessor for someStr) from CompanionClass$Companion to CompanionClass
// as a static method.
class CompanionClass {
  companion object {
    var someStr: String = "Foo"
  }

  fun get(): String {
    return someStr
  }
}

// Named companion object with @JvmStatic: the pass relocates getSomeOtherStr
// and funX from AnotherCompanionClass$Test to AnotherCompanionClass as static
// methods.
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

// Method name collision: both the outer class and the companion define get().
// When the pass relocates the companion's get() to the outer class, it must
// rename it to "get$CompanionWithMethodCollision$Companion" to avoid colliding
// with the outer class's existing get().
class CompanionWithMethodCollision {
  companion object {
    fun get(): String = "test2"
  }

  fun get(): String = "test1"
}

class CollisionTestCaller {
  fun main() {
    val obj = CompanionWithMethodCollision()
    print(obj.get())
    print(CompanionWithMethodCollision.get())
  }
}
