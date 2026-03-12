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

// Second companion object with @JvmStatic: the pass relocates getSomeOtherStr
// and funX from AnotherCompanionClass$Companion to AnotherCompanionClass as
// static methods.
class AnotherCompanionClass {
  companion object {
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

// Named companion object: the pass should NOT relocate methods from
// NamedCompanionClass$Custom because it does not end with $Companion.
class NamedCompanionClass {
  companion object Custom {
    var someStr: String = "Baz"

    fun funY(): String {
      return someStr
    }
  }
}

class NamedCompanionCaller {
  fun main() {
    print(NamedCompanionClass.someStr)
    print(NamedCompanionClass.funY())
  }
}

// Companion methods calling each other: exercises rewrite_this_calls_to_static
// and two-pass relocation. methodA calls methodB via `this`, and methodB calls
// methodA via `this`. Both should be relocated as static methods.
class CompanionWithInterCalls {
  companion object {
    fun methodA(x: Int): Int {
      return if (x > 0) methodB(x - 1) else x
    }

    fun methodB(x: Int): Int {
      return if (x > 1) methodA(x - 2) else x
    }
  }
}

class InterCallsCaller {
  fun main() {
    print(CompanionWithInterCalls.methodA(5))
    print(CompanionWithInterCalls.methodB(3))
  }
}

// @JvmStatic bridge rename: the companion method compute() has @JvmStatic,
// which generates a static bridge on the outer class. After KeepThis::No
// drops the this parameter, the companion method's proto matches the bridge.
// The pass must rename the bridge to compute$companion_bridge to free the name.
class CompanionWithJvmStaticBridge {
  companion object {
    @JvmStatic fun compute(x: Int): Int = x * 2
  }
}

class JvmStaticBridgeCaller {
  fun main() {
    print(CompanionWithJvmStaticBridge.compute(42))
  }
}

// Companion method with default arguments: Kotlin generates a static $default
// method on the companion class. This method takes the companion instance as
// its first parameter (it's already static), and the compiler reuses that
// register for the AND_INT_LIT bitmask check.  The pass must handle this
// without corrupting the $default method's CFG.
class CompanionWithDefaults {
  companion object {
    fun greet(name: String, greeting: String = "Hello"): String {
      return "$greeting, $name!"
    }
  }
}

class DefaultArgsCaller {
  fun main() {
    // Uses default value for greeting — generates call to greet$default
    print(CompanionWithDefaults.greet("World"))
    // Explicit value — direct call to greet
    print(CompanionWithDefaults.greet("World", "Hi"))
  }
}
