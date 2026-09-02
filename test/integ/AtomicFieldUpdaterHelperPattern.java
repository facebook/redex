/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;

public class AtomicFieldUpdaterHelperPattern {

  public static class Holder {
    volatile Object ref;
  }

  public static class Helper {
    static final AtomicReferenceFieldUpdater<Holder, Object> REF =
        AtomicReferenceFieldUpdater.newUpdater(Holder.class, Object.class, "ref");
  }

  public static Object helperFieldPattern(Holder h) {
    return Helper.REF.get(h);
  }
}
