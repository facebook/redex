/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Test bitset interprocedural constant propagation with recursive use of bitset.
// This extracts the core of bitset constant propagation in Composables without the rest of
// Composable noises.
class RecursiveBitsetConstantPropagation {
  // flags plays a similar role to defaults in Composable, except not reused with a lambda.
  private fun processWithNoLambda(count: Int, flags: Int) {
    if (flags and 1 != 0) {
      print("Lowest bit is set")
    }
    if (flags and 2 != 0) {
      print("Second lowest bit is set")
    }
    if (count == 0) {
      return
    }
    processWithNoLambda(count - 1, flags)
  }

  // flags plays a similar role to defaults in Composable. It is reused from within a lambda.
  private fun processWithLambda(count: Int, flags: Int) {
    if (flags and 1 != 0) {
      print("Lowest bit is set")
    }
    if (flags and 2 != 0) {
      print("Second lowest bit is set")
    }
    if (count == 0) {
      return
    }
    // flags may be reused by an external caller calling callLambda.
    takeALambda { processWithLambda(count - 1, flags) }
  }

  private fun takeALambda(f: () -> Unit) {
    this.f = f
  }

  private var f = { -> Unit }

  fun callLambda() {
    f()
  }

  fun main() {
    // Second lowest bit of flags is never set.
    processWithNoLambda(1, 1)
    processWithNoLambda(2, 1)
    processWithLambda(1, 1)
    processWithLambda(2, 1)
  }
}
