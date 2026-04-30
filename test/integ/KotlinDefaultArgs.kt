/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class KotlinDefaultArgs {
  private fun greet(name: String = "Guest", greeting: String = "Hello") {
    print("$greeting, $name!")
  }

  public fun main() {
    // Never called greet with the default greeting
    greet(greeting = "Hi") // Uses default name: "Hi, Guest!"
    greet("Bob", "Welcome") // Uses provided arguments: "Welcome, Bob!"
  }
}
