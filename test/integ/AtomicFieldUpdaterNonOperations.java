/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import androidx.annotation.RequiresApi;
import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;

/**
 * Calls against an updater that are not operations the pass models, next to one that is. The
 * updater itself is recognizable, so what these exercise is the classifier rather than recognition.
 *
 * <p>`getAndUpdate` is a real operation the pass cannot lower -- its trailing argument is an
 * operator, so the value written is only known at runtime -- while `toString` and `equals` are not
 * operations at all, merely methods every object inherits. The census counts the first and ignores
 * the last two; neither is rewritable.
 */
public class AtomicFieldUpdaterNonOperations {

  public static class Holder {
    volatile Object ref;

    static final AtomicReferenceFieldUpdater<Holder, Object> REF =
        AtomicReferenceFieldUpdater.newUpdater(Holder.class, Object.class, "ref");
  }

  // `getAndUpdate` is API 24. `@RequiresApi` rather than `@TargetApi` so the
  // requirement propagates to any caller instead of being suppressed here; this
  // fixture is compiled to dex and handed to the pass, never run.
  @RequiresApi(24)
  public static Object functionalForm(Holder h) {
    return Holder.REF.getAndUpdate(h, v -> v);
  }

  public static String inheritedMethods(Holder h) {
    boolean eq = Holder.REF.equals(h);
    return Holder.REF.toString() + eq;
  }
}
