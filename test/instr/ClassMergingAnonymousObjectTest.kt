/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

@file:Suppress("IncorrectPackageName")

package com.facebook.redextest

import com.facebook.redex.test.instr.KeepForRedexTest
import org.assertj.core.api.Assertions.assertThat
import org.junit.Test

abstract class AbstractBase {}

@KeepForRedexTest
class ClassMergingAnonymousObjectTest {

  open class BaseA : AbstractBase() {
    open fun method(): String {
      return "BaseA"
    }
  }

  open class SubA1 : BaseA() {
    override fun method(): String {
      return "SubA1"
    }
  }

  class SubA2 : BaseA() {
    override fun method(): String {
      return "SubA2"
    }
  }

  class SubA3 : BaseA() {
    override fun method(): String {
      return "SubA3"
    }
  }

  val getSubA1 = object : SubA1() {}
  val getSubA12 = object : SubA1() {}

  @KeepForRedexTest
  @Test
  fun testOneImpleBaseCls() {
    val s1 = SubA1()
    val s2 = SubA2()
    val s3 = getSubA1
    val s4 = getSubA12
    val s5 = SubA3()

    assertThat(s1.method()).isEqualTo("SubA1")
    assertThat(s2.method()).isEqualTo("SubA2")
    assertThat(s3.method()).isEqualTo("SubA1")
    assertThat(s4.method()).isEqualTo("SubA1")
    assertThat(s5.method()).isEqualTo("SubA3")
  }
}
