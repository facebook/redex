/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest

import java.util.concurrent.atomic.AtomicReferenceFieldUpdater

/**
 * The shape accessor inlining exists for, produced by kotlinc rather than written out by hand.
 *
 * A private updater held in a companion object is not read directly at the call site: Kotlin emits
 * a static getter for it, and adds an `access$` bridge on top when the read comes from a nested
 * class. So the receiver of the operation is defined by an invoke-static, never by the sget-object
 * that resolution looks for, and nothing here is rewritable until those are flattened.
 */
class AtomicFieldUpdaterAccessors {
  @Volatile @JvmField var next: Any? = null

  companion object {
    private val U: AtomicReferenceFieldUpdater<AtomicFieldUpdaterAccessors, Any> =
        AtomicReferenceFieldUpdater.newUpdater(
            AtomicFieldUpdaterAccessors::class.java,
            Any::class.java,
            "next",
        )

    // Read from the companion itself: reaches the field through the getter.
    @JvmStatic
    fun setFromCompanion(h: AtomicFieldUpdaterAccessors, v: Any) {
      U.set(h, v)
    }
  }

  /** Read from a nested class, which is what puts an `access$` bridge in front of the getter. */
  class Nested {
    fun getFromNested(h: AtomicFieldUpdaterAccessors): Any? {
      return U.get(h)
    }
  }
}
