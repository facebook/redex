/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redexlinemap

@Suppress("NOTHING_TO_INLINE")
inline fun kotlinInlineThrower() {
  throw RuntimeException("from Kotlin inline function")
}

@NoInline
fun kotlinRegularThrower() {
  throw RuntimeException("from regular function called by Kotlin inline function")
}

@Suppress("NOTHING_TO_INLINE")
inline fun kotlinInlineCallsRegularThrower() {
  kotlinRegularThrower()
}

@Suppress("NOTHING_TO_INLINE")
inline fun kotlinNestedInlineCallsRegularThrower() {
  kotlinInlineCallsRegularThrower()
}
