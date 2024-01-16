/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

class TypeAnalysisRemoveRedundantNullCheck {
  fun foo(str: String) {
    print(str)
  }

  fun main() {
    foo("test")
  }
}
