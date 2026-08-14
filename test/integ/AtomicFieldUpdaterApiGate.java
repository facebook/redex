/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.concurrent.atomic.AtomicIntegerFieldUpdater;

/**
 * A single API-gated operation. `Unsafe.getAndSetInt` exists only from Android N, so below that
 * min_sdk this site must be counted and left alone rather than lowered.
 *
 * <p>The instrumentation test covers the other side of the boundary -- it builds at min_sdk 24 and
 * asserts on device that these operations are both rewritten and correct -- which is why nothing
 * here needs to assert the API-24 case.
 */
public class AtomicFieldUpdaterApiGate {

  public static class Holder {
    volatile int i;

    static final AtomicIntegerFieldUpdater<Holder> I =
        AtomicIntegerFieldUpdater.newUpdater(Holder.class, "i");
  }

  public static int swap(Holder h) {
    return Holder.I.getAndSet(h, 7);
  }
}
