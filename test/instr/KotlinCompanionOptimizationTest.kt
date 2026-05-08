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
  companion object {
    @JvmStatic var someOtherStr: String = "Bar"

    @JvmStatic
    fun funX(): String {
      return someOtherStr
    }
  }
}

class ThirdCompanionClass {
  companion object {
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

// Named companion object — must NOT be relocated by the pass because its
// inner class name (NamedCompanionClass$Custom) does not end with $Companion.
class NamedCompanionClass {
  companion object Custom {
    var someStr: String = "Baz"

    fun funZ(): String {
      return someStr
    }
  }
}

// Companion with const val: the companion has a <clinit> that initializes
// $$INSTANCE.  The clinit produces sget-object/sput-object which must be
// allowlisted in is_def_trackable.
class CompanionWithConstVal {
  companion object {
    const val MAGIC = 42

    fun getMagic(): Int = MAGIC
  }
}

// Method name collision: both the outer class and the companion define get().
// The pass must rename the relocated companion method to avoid collision.
class CompanionWithMethodCollision {
  companion object {
    fun get(): String = "companion"
  }

  fun get(): String = "outer"
}

// Companion methods calling each other via `this`.
class CompanionWithInterCalls {
  companion object {
    fun methodA(x: Int): Int = if (x > 0) methodB(x - 1) else x

    fun methodB(x: Int): Int = if (x > 1) methodA(x - 2) else x
  }
}

// @JvmStatic bridge: the companion method compute() has @JvmStatic, which
// generates a static bridge on the outer class with the same name/proto.
class CompanionWithJvmStaticBridge {
  companion object {
    @JvmStatic fun compute(x: Int): Int = x * 2
  }
}

// Companion method with default arguments.
class CompanionWithDefaults {
  companion object {
    fun greet(name: String, greeting: String = "Hello"): String {
      return "$greeting, $name!"
    }
  }
}

// Abstract outer class with a companion object.
abstract class AbstractOuterClass {
  companion object {
    fun helperFunc(): String = "abstract_helper"
  }

  abstract fun doWork(): String
}

class ConcreteSubclass : AbstractOuterClass() {
  override fun doWork(): String = helperFunc()
}

// Companion with a pure function that takes a parameter.
class CompanionWithPureFunction {
  companion object {
    fun double(x: Int): Int = x * 2
  }
}

// Companion whose instance escapes via a function return.
class CompanionEscapes {
  companion object {
    fun doWork(): String = "work"
  }
}

fun getCompanion(): CompanionEscapes.Companion = CompanionEscapes.Companion

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

    println(NamedCompanionClass.funZ())

    println(CompanionWithConstVal.getMagic())

    // New test classes
    val collision = CompanionWithMethodCollision()
    println(collision.get())
    println(CompanionWithMethodCollision.get())

    println(CompanionWithInterCalls.methodA(5))
    println(CompanionWithInterCalls.methodB(3))

    println(CompanionWithJvmStaticBridge.compute(42))

    println(CompanionWithDefaults.greet("World"))
    println(CompanionWithDefaults.greet("World", "Hi"))

    println(AbstractOuterClass.helperFunc())
    println(ConcreteSubclass().doWork())

    println(CompanionWithPureFunction.double(21))

    println(CompanionEscapes.doWork())
    println(getCompanion())
  }
}
