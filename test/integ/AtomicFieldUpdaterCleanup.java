/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;

public class AtomicFieldUpdaterCleanup {

  public static class FullyRewrittenHolder {
    volatile Object ref;

    static final AtomicReferenceFieldUpdater<FullyRewrittenHolder, Object> REF =
        AtomicReferenceFieldUpdater.newUpdater(
            FullyRewrittenHolder.class, Object.class, "ref");
  }

  public static class PartiallyRewrittenHolder {
    volatile Object ref;

    static final AtomicReferenceFieldUpdater<PartiallyRewrittenHolder, Object> REF =
        AtomicReferenceFieldUpdater.newUpdater(
            PartiallyRewrittenHolder.class, Object.class, "ref");
  }

  public static class UnrewrittenHolder {
    volatile Object ref;

    static final AtomicReferenceFieldUpdater<UnrewrittenHolder, Object> REF =
        AtomicReferenceFieldUpdater.newUpdater(
            UnrewrittenHolder.class, Object.class, "ref");
  }

  public static Object fullyRewritten(FullyRewrittenHolder h) {
    return FullyRewrittenHolder.REF.get(h);
  }

  public static Object partiallyRewritten(PartiallyRewrittenHolder h) {
    return PartiallyRewrittenHolder.REF.get(h);
  }

  public static String partialKeepsUpdater() {
    return PartiallyRewrittenHolder.REF.toString();
  }

  public static String unrewrittenOnly() {
    return UnrewrittenHolder.REF.toString();
  }
}
