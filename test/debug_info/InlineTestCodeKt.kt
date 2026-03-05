/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap

class InlineTestCodeKt {
  companion object {
    @JvmStatic
    @NoInline
    fun withPrecond(s: String): Boolean {
      throw RuntimeException(s)
    }
  }
}
