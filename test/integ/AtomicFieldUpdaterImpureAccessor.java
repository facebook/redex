/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;

/**
 * A pure accessor chain the pass should flatten, beside a method that merely happens to return the
 * same updater and must be left alone.
 *
 * <p>The chain is two deep on purpose: selection reaches a fixed point, so the bridge only becomes
 * eligible in the round after the getter it delegates to. That the impure method is re-examined in
 * every one of those rounds is what makes it worth asserting it is counted once rather than once
 * per round.
 */
public class AtomicFieldUpdaterImpureAccessor {

  public static int sideEffect;

  public static class Holder {
    volatile Object ref;

    static final AtomicReferenceFieldUpdater<Holder, Object> U =
        AtomicReferenceFieldUpdater.newUpdater(Holder.class, Object.class, "ref");

    private static AtomicReferenceFieldUpdater<Holder, Object> getU() {
      return U;
    }

    static AtomicReferenceFieldUpdater<Holder, Object> bridgeU() {
      return getU();
    }

    /** Same signature, same return value, but observable: inlining it is out of scope. */
    static AtomicReferenceFieldUpdater<Holder, Object> logAndGetU() {
      sideEffect++;
      return U;
    }
  }

  public static Object throughChain(Holder h) {
    return Holder.bridgeU().get(h);
  }

  public static Object throughImpure(Holder h) {
    return Holder.logAndGetU().get(h);
  }
}
