/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest


import integ.TestStringDef

public class TypedefAnnoCheckerKtTest {

  fun testSynthAccessor() {
    val lmd: () -> String = { takesStrConst("liu") }
    lmd()
  }

  private fun takesStrConst(@TestStringDef str: String): String {
    return str
  }
}
