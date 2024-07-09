/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest

import integ.TestIntDef
import integ.TestStringDef

public class ClassWithParams(@TestIntDef var int_field: Int) {}

public class ClassWithDefaultParams(@TestIntDef var int_field: Int = TestIntDef.THREE) {}

public class TypedefAnnoCheckerKtTest {

  @TestStringDef val field_str: String = TestStringDef.TWO
  @TestStringDef var var_field: String = TestStringDef.THREE
  @NotSafeAnno val field_not_safe: String = "4"
  @TestIntDef var field_int: Int = TestIntDef.ONE

  companion object {
    @TestStringDef val companion_val: String = TestStringDef.ONE
    @TestStringDef var companion_var: String = TestStringDef.FOUR
    @TestIntDef var companion_int_var: Int = TestIntDef.FOUR
  }

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
  fun testCompanionObject(): String {
    return companion_val
  }

  @TestStringDef
  fun testCompanionVarSetter(): String {
    companion_var = TestStringDef.ONE
    return companion_var
  }

  @TestIntDef
  fun testCompanionIntVarSetter(): Int {
    companion_int_var = TestIntDef.ONE
    return companion_int_var
  }

  @TestStringDef
  fun testInvalidCompanionVarSetter(): String {
    companion_var = "5"
    return companion_var
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

  @TestStringDef
  fun call_lambda(lambda_func: () -> String): String {
    return lambda_func()
  }

  fun assign_and_print(@TestStringDef param: String) {
    var_field = param
    print(param)
  }

  fun assign_and_print_default(@TestStringDef param: String, def_param: String = "hello") {
    print(param)
    print(def_param)
  }

  @TestStringDef
  fun testLambdaCall(@TestStringDef param: String): String {
    return call_lambda({
      var_field = param
      assign_and_print(param)
      param
    })
  }

  @TestIntDef
  fun testClassConstructorArgs(@TestIntDef param: Int): Int {
    val ctor_test: ClassWithParams = ClassWithParams(param)
    return ctor_test.int_field
  }

  @TestIntDef
  fun testClassConstructorDefaultArgs(@TestIntDef param: Int): Int {
    val default_ctor_test: ClassWithDefaultParams = ClassWithDefaultParams(param)
    return default_ctor_test.int_field
  }

  @TestStringDef
  fun testLambdaCallLocalVar(): String {
    val local_val = "two"
    return call_lambda({
      assign_and_print_default(local_val)
      local_val
    })
  }

  @TestStringDef
  fun testLambdaCallLocalVarInvalid(): String {
    val local_val = "randomval"
    return call_lambda({
      assign_and_print_default(local_val)
      local_val
    })
  }

  ////////////////////////////////////////////////////////////////////////////////////////////
  interface Error {
    @get:TestStringDef val failure: String
  }

  abstract class BaseError : Error {}

  class Mismatch : BaseError() {
    @TestStringDef override val failure: String = TestStringDef.ONE
  }

  class Blowup : BaseError() {
    @TestStringDef override val failure: String = TestStringDef.THREE
  }

  fun getError(): Error {
    return Mismatch()
  }

  @TestStringDef
  fun testAnnotatedPropertyGetterPatching(): String {
    val e = getError()
    return e.failure
  }

  ////////////////////////////////////////////////////////////////////////////////////////////
  class Listener {
    companion object {
      @TestStringDef private val ONE: String = TestStringDef.ONE

      @TestStringDef
      fun getOne(): String {
        return ONE
      }
    }
  }

  // This test method is just a placehold to make the test look complete. The real testing target is
  // the getOne() method in the Companion object above.
  @TestStringDef
  fun testAnnotatedCompanionPropertyAccessorPatching(): String {
    return Listener.getOne()
  }
}
