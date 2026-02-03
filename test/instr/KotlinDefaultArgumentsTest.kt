/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

@file:Suppress("IncorrectPackageName")

package redex

// CHECK: method: direct redex.KotlinDefaultArgumentsTestKt.greet$default
// CHECK: const-string v{{[0-9]+}}, "Guest"
// greet is never called with the default greeting. The "Hello" branch is optimized out.
// PRECHECK: if-eqz{{.*}}
// POSTCHECK-NOT: if-eqz{{.*}}
// PRECHECK: const-string v{{[0-9]+}}, "Hello"
// POSTCHECK-NOT: const-string v{{[0-9]+}}, "Hello"

// CHECK: method: direct redex.KotlinDefaultArgumentsTestKt.greet
private fun greet(name: String = "Guest", greeting: String = "Hello") {
  print("$greeting, $name!")
}

// CHECK: method: direct redex.KotlinDefaultArgumentsTestKt.hi
fun hi() {
  // CHECK: invoke-static {{.*}} redex.KotlinDefaultArgumentsTestKt.greet$default
  greet(greeting = "Hi") // Uses default name: "Hi, Guest!"
  // CHECK: invoke-static {{.*}} redex.KotlinDefaultArgumentsTestKt.greet
  greet("Bob", "Welcome") // Uses provided arguments: "Welcome, Bob!"
}

// CHECK: method: direct redex.KotlinDefaultArgumentsTestKt.bonjour
fun bonjour() {
  // CHECK: invoke-static {{.*}} redex.KotlinDefaultArgumentsTestKt.greet$default
  greet(greeting = "Bonjour") // Uses default name: "Bonjour, Guest!"
  // CHECK: invoke-static {{.*}} redex.KotlinDefaultArgumentsTestKt.greet
  greet("Bob", "Bienvenue") // Uses provided arguments: "Bienvenue, Bob!"
}
