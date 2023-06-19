/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

import androidx.annotation.IntDef

/* Note: companion object in annotation class will be removed by AnnoKill + RMU pass, NOT KotlinObjectInliner pass*/
@IntDef(AnnoClass.START, AnnoClass.END)
annotation class AnnoClass {
  companion object {
    const val START = 0
    const val END = 1
  }
}

/* After KotlinObjectInliner + LocalDce + RMU, class LCompanionClass$Companion; should be removed*/
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
  }
}
