/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;

/**
 * A getter that hands back *another* class's updater.
 *
 * <p>It is pure, and inlining it would preserve semantics, but it is not the shape this pass
 * flattens. A synthetic accessor is always emitted in the class that owns the private member it
 * reaches, so an accessor reading a field of some other class is hand-written code rather than
 * codegen standing in the way -- and reading it can run a `<clinit>` that calling the accessor did
 * not, which is exactly what the same-class requirement rules out.
 */
public class AtomicFieldUpdaterCrossClassAccessor {

  public static class Holder {
    volatile Object ref;

    static final AtomicReferenceFieldUpdater<Holder, Object> U =
        AtomicReferenceFieldUpdater.newUpdater(Holder.class, Object.class, "ref");
  }

  /** Same shape as a synthetic getter, but declared away from the field it reads. */
  static AtomicReferenceFieldUpdater<Holder, Object> crossClassU() {
    return Holder.U;
  }

  public static Object throughCrossClass(Holder h) {
    return crossClassU().get(h);
  }
}
