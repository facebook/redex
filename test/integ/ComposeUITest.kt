/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// An instrumentation test version of this file is also available with the same name.

@file:Suppress("IncorrectPackageName")

package redex

import androidx.compose.runtime.Composable

@Composable
private fun SuperTextPrinter(supertext: String, subtext: String = getTestDefault()) {
  println("Super Text: $supertext")
  SubTextPrinter(subtext)
}

@Composable
private fun SubTextPrinter(text: String) {
  println("Sub Text: $text")
}

@Composable
private fun getTestDefault(): String {
  return "Test Default"
}

@Composable
fun HelloWorldText() {
  // subtext default is never used.
  SuperTextPrinter("English", "Hello World")
}
