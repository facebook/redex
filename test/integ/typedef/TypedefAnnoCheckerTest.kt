/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest

import integ.TestIntDef
import integ.TestStringDef

fun interface SAMInterface {
  fun setString(@TestStringDef sam_string: String)
}

public class ClassWithParams(@TestIntDef var int_field: Int) {}

public class ClassWithDefaultParams(@TestIntDef var int_field: Int = TestIntDef.THREE) {}

public class TypedefAnnoCheckerKtTest {

  @TestStringDef val field_str: String = TestStringDef.TWO
  @TestStringDef var var_field: String = TestStringDef.THREE
  @NotSafeAnno val field_not_safe: String = "4"
  @TestIntDef var field_int: Int = TestIntDef.ONE
  private val sam_interface = SAMInterface { sam_string -> var_field = sam_string }

  companion object {
    @TestStringDef val companion_val: String = TestStringDef.ONE
    @TestStringDef var companion_var: String = TestStringDef.FOUR
    @TestIntDef var companion_int_var: Int = TestIntDef.FOUR

    @TestStringDef
    fun testCompanionObjectGetter(): String {
      return companion_val
    }
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

  @TestIntDef
  fun call_lambda_int(lambda_func: () -> Int): Int {
    return lambda_func()
  }

  fun assign_and_print_int(@TestIntDef param: Int) {
    print(param)
  }

  fun assign_and_print_default_int(@TestIntDef param: Int, def_param: String = "hello") {
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
  fun testLambdaCallLocalVal(): String {
    val local_val = "two"
    return call_lambda({
      assign_and_print_default(local_val)
      local_val
    })
  }

  @TestStringDef
  fun testLambdaCallLocalValInvalid(): String {
    val local_val = "randomval"
    return call_lambda({
      assign_and_print_default(local_val)
      local_val
    })
  }

  @TestStringDef
  fun testLambdaCallLocalVarString(): String {
    var local_var: String = "two"
    if (var_field == "one") {
      local_var = "one"
    }
    return call_lambda({
      assign_and_print(local_var)
      local_var
    })
  }

  @TestStringDef
  fun testLambdaCallLocalVarStringInvalid(): String {
    var local_var: String = "seven"
    if (var_field == "one") {
      local_var = "eight"
    }
    return call_lambda({
      assign_and_print(local_var)
      local_var
    })
  }

  @TestStringDef
  fun testLambdaCallLocalVarStringDefault(): String {
    var local_var: String = "two"
    if (var_field == "one") {
      local_var = "one"
    }
    return call_lambda({
      assign_and_print_default(local_var)
      local_var
    })
  }

  @TestStringDef
  fun testLambdaCallLocalVarStringDefaultInvalid(): String {
    var local_var: String = "seven"
    if (var_field == "one") {
      local_var = "eight"
    }
    return call_lambda({
      assign_and_print_default(local_var)
      local_var
    })
  }

  @TestIntDef
  fun testLambdaCallLocalVarInt(): Int {
    var local_var: Int = 2
    if (var_field == "one") {
      local_var = 1
    }
    return call_lambda_int({
      assign_and_print_int(local_var)
      local_var
    })
  }

  @TestIntDef
  fun testLambdaCallLocalVarIntInvalid(): Int {
    var local_var: Int = 7
    if (var_field == "one") {
      local_var = 9
    }
    return call_lambda_int({
      assign_and_print_int(local_var)
      local_var
    })
  }

  @TestIntDef
  fun testLambdaCallLocalVarIntDefault(): Int {
    var local_var: Int = 2
    if (var_field == "one") {
      local_var = 1
    }
    return call_lambda_int({
      assign_and_print_default_int(local_var)
      local_var
    })
  }

  @TestIntDef
  fun testLambdaCallLocalVarIntDefaultInvalid(): Int {
    var local_var: Int = 7
    if (var_field == "one") {
      local_var = 9
    }
    return call_lambda_int({
      assign_and_print_default_int(local_var)
      local_var
    })
  }

  ////////////////////////////////////////////////////////////////////////////////////////////
  interface Error {
    @get:TestStringDef var failure: String
  }

  abstract class BaseError : Error {}

  class Mismatch : BaseError() {
    @TestStringDef override var failure: String = TestStringDef.ONE
  }

  fun getError(): Error {
    return Mismatch()
  }

  @TestStringDef
  fun testAnnotatedPropertyGetterPatching(): String {
    val e = getError()
    return e.failure
  }

  fun testAnnotatedPropertySetterPatching() {
    val e = getError()
    e.failure = TestStringDef.TWO
  }

  ////////////////////////////////////////////////////////////////////////////////////////////
  class Listener {
    companion object {
      @TestStringDef private var ONE: String = TestStringDef.ONE

      @TestStringDef
      fun getOne(): String {
        return ONE
      }

      fun setOne(@TestStringDef param: String) {
        ONE = param
      }
    }
  }

  // These test methods are just placeholders to make the test look complete. The real testing
  // targets are
  // the getOne() and setOne() methods in the Companion object above.
  @TestStringDef
  fun testAnnotatedCompanionPropertyAccessorGetter(): String {
    return Listener.getOne()
  }

  fun testAnnotatedCompanionPropertyAccessorSetter() {
    Listener.setOne(TestStringDef.TWO)
  }

  ////////////////////////////////////////////////////////////////////////////////////////////
  public class ClassWithPrivateProperty(@TestIntDef private var int_field: Int) {

    fun printInt(@TestIntDef param: Int) {
      print(param)
    }

    @TestIntDef
    fun returnInt(): Int {
      val lmd: () -> (Unit) = { printInt(int_field) }
      lmd()
      return int_field
    }

    fun setInt() {
      val lmd: () -> (Unit) = {
        int_field = TestIntDef.TWO
        printInt(int_field)
      }
      lmd()
    }
  }

  ////////////////////////////////////////////////////////////////////////////////////////////
  ////////// fun interface tests /////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////
  fun interface OnListener {
    fun onListen()
  }

  fun setOnListener(listener: OnListener) {
    listener.onListen()
  }

  @TestStringDef
  fun getTestString(): String {
    return "one"
  }

  fun testFunInterface() {
    val constVal = getTestString()
    setOnListener({ wrongDefaultCaller(constVal) })
  }
}
