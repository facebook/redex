/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.concurrent.atomic.AtomicIntegerFieldUpdater;
import java.util.concurrent.atomic.AtomicLongFieldUpdater;
import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;

/**
 * Two integration tests compile this fixture and assert against its exact
 * contents: AtomicFieldUpdaterLoweringTest, on what the lowering produces, and
 * AtomicFieldUpdaterKotlinStats, on how many updaters and operations
 * PrintKotlinStats counts. Adding or removing a call site here changes the
 * counts the latter expects, and the failure will not name this file.
 */
public class AtomicFieldUpdaterLowering {

  public static class Holder {
    volatile Object ref;
    volatile int i;
    volatile long l;

    static final AtomicReferenceFieldUpdater<Holder, Object> REF =
        AtomicReferenceFieldUpdater.newUpdater(Holder.class, Object.class, "ref");
    static final AtomicIntegerFieldUpdater<Holder> I =
        AtomicIntegerFieldUpdater.newUpdater(Holder.class, "i");
    static final AtomicLongFieldUpdater<Holder> L = AtomicLongFieldUpdater.newUpdater(Holder.class, "l");
  }

  // The holder is a fresh instance, so non-nullness is provable and the
  // rewrite needs no guard.
  public static Object provenHolder() {
    Holder h = new Holder();
    Holder.REF.set(h, "a");
    Holder.REF.compareAndSet(h, "a", "b");
    return Holder.REF.get(h);
  }

  // The Integer and Long flavors, including a wide value.
  public static long primitives() {
    Holder h = new Holder();
    Holder.I.set(h, 1);
    Holder.I.compareAndSet(h, 1, 2);
    Holder.L.set(h, 3L);
    return Holder.I.get(h) + Holder.L.get(h);
  }

  // The holder arrives as a parameter, so nothing proves it non-null: the
  // rewrite has to carry an explicit ClassCastException check.
  public static Object unprovenHolder(Holder h) {
    return Holder.REF.get(h);
  }

  // The updater itself arrives as a parameter, so it cannot be traced back to
  // a static field and there is no offset to substitute.
  public static Object unresolvableUpdater(
      AtomicReferenceFieldUpdater<Holder, Object> u, Holder h) {
    return u.get(h);
  }
}
