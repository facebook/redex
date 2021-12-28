/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class TypeAnalysisRemoveRedundantCmp {
  private val data: Array<Any> = arrayOf(1, "1")
  val x
    get() = data[0] as Int
  val y
    get() = data[1] as String

  private val dataMayBeNull: Array<Any?> = arrayOf(1, null)
  val xx
    get() = dataMayBeNull[0] as Int
  val yy
    get() = dataMayBeNull[1] as String

  fun main() {
    println(TypeAnalysisRemoveRedundantCmp().x)
    println(TypeAnalysisRemoveRedundantCmp().yy)
  }
}
