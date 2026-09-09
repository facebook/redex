/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;

/**
 * A single API-gated operation. `Unsafe.getAndSetObject` exists only from Android N, so below that
 * min_sdk this site must be counted and left alone rather than lowered.
 *
 * <p>The reference flavor specifically: `getAndSetInt` and `getAndSetLong` would reach the
 * hidden-API gate first, since Android restricts those to apps targeting API 30 or below, and this
 * fixture would then stop exercising the min_sdk gate it exists for. `getAndSetObject` carries no
 * such restriction, so min_sdk is the only thing standing between it and a rewrite.
 *
 * <p>The instrumentation test covers the other side of the boundary -- it builds at min_sdk 24 and
 * asserts on device that these operations are both rewritten and correct -- which is why nothing
 * here needs to assert the API-24 case.
 */
public class AtomicFieldUpdaterApiGate {

  public static class Holder {
    volatile Object ref;

    static final AtomicReferenceFieldUpdater<Holder, Object> REF =
        AtomicReferenceFieldUpdater.newUpdater(Holder.class, Object.class, "ref");
  }

  public static Object swap(Holder h) {
    return Holder.REF.getAndSet(h, "seven");
  }
}
