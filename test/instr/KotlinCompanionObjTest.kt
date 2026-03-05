/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import androidx.annotation.IntDef

/* Note: companion object in annotation class will be removed by AnnoKill + RMU pass, NOT KotlinCompanionOptimizationPass*/
@IntDef(AnnoClass.START, AnnoClass.END)
annotation class AnnoClass {
  companion object {
    const val START = 0
    const val END = 1
  }
}

/* After KotlinCompanionOptimizationPass + LocalDce + RMU, class LCompanionClass$Companion; should be removed*/
class CompanionClass(val greeting: String) {
  fun greet(name: String) = "$greeting, $name!"

  companion object {
    val s: String = "Hello"
    var s1 = "Hello2"

    fun hello() = CompanionClass(s)

    fun hello1(): Boolean {
      if (s1 == "Hello2") {
        return true
      } else {
        return false
      }
    }

    fun get(): String {
      return s1
    }
  }

  fun get(): String {
    return s
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

class ThirdCompanionClass {
  companion object Test {
    const val thirdStr: String = "Bar"

    private fun funY(): String {
      return thirdStr
    }
  }

  fun get(): String {
    return funY()
  }
}

// Named object declaration (singleton) — must NOT be optimized by this pass
object NamedObjectDeclaration {
  var value: Int = 0

  fun increment() {
    value++
  }
}

// Nested named object declaration — must NOT be optimized by this pass
class OuterWithObject {
  object NestedObj {
    var data: String = "test"
  }

  fun useNested(): String = NestedObj.data
}

// Companion with outer-class sfields (mutable var, not const) — optimized
// (the backing field lives on the outer class, not on the companion)
class CompanionWithSfields {
  companion object {
    var counter: Int = 0

    fun increment() {
      counter++
    }
  }
}

// Companion with outer-class <clinit> (lazy init / computed field) — optimized
// (the <clinit> and backing field live on the outer class, not on the companion)
class CompanionWithClinit {
  companion object {
    val computed: String = buildString {
      append("Hello")
      append("World")
    }
  }
}

class Foo {
  fun main() {

    println(CompanionClass.hello().greet("Olive"))
    println(CompanionClass.s)
    println(CompanionClass.s1)
    println(CompanionClass.hello1())
    println(CompanionClass.get())
    println(AnotherCompanionClass.funX())
    println(AnnoClass.START)

    val obj = ThirdCompanionClass()
    println(obj.get())

    CompanionWithSfields.increment()
    println(CompanionWithSfields.counter)

    println(CompanionWithClinit.computed)

    NamedObjectDeclaration.increment()
    println(NamedObjectDeclaration.value)

    println(OuterWithObject().useNested())
  }
}
