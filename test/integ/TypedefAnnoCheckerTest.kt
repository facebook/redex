/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest

import integ.TestIntDef
import integ.TestStringDef

annotation class NotSafeAnno {}

public class TypedefAnnoCheckerKtTest {

  @TestStringDef val field_str: String = TestStringDef.TWO
  @TestStringDef var var_field: String = TestStringDef.THREE
  @NotSafeAnno val field_not_safe: String = "4"

  fun testSynthAccessor() {
    val lmd: () -> String = { takesStrConst("liu") }
    lmd()
  }

  private fun takesStrConst(@TestStringDef str: String): String {
    return str
  }

  fun wrongDefaultCaller(@TestStringDef arg: String) {
    wrongDefaultArg(arg)
    wrongDefaultArg()
  }

  private fun wrongDefaultArg(@TestStringDef str: String = "default"): String {
    return str
  }

  fun rightDefaultCaller(@TestStringDef arg: String) {
    rightDefaultArg(arg)
    rightDefaultArg()
  }

  private fun rightDefaultArg(@TestStringDef str: String = "one"): String {
    return str
  }

  @TestStringDef
  fun testKtField(): String {
    return field_str
  }

  @TestStringDef
  fun testVarField(): String {
    var_field = TestStringDef.ONE
    return var_field
  }

  @TestStringDef
  fun testInvalidVarField(): String {
    var_field = "5"
    return var_field
  }

  @TestIntDef
  fun testReturnWhen(): Int {
    return when (field_str) {
      "1" -> 1
      "2" -> 2
      "3" -> 3
      "4" -> 4
      else -> 0
    }
  }
}
