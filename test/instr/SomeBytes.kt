/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package redex

val COLOR_BYTE: Byte = UByte.MAX_VALUE.toByte()

fun fillArray(): ByteArray {
  val a = ByteArray(100)
  a.fill(COLOR_BYTE)
  if (a[0] != COLOR_BYTE) {
    throw IllegalStateException(a[0].toString())
  }
  return a
}
