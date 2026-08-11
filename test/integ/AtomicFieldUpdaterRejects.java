/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redextest;

import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;

/**
 * Shapes recognition must refuse. Every updater here is well-formed enough to compile and to look
 * like a candidate; each is rejected for a different reason, and the test asserts that none is
 * recognized. Keep this fixture free of recognizable updaters -- the assertion is a count over the
 * whole program.
 */
public class AtomicFieldUpdaterRejects {

  /**
   * The named field is not volatile. The updater's contract rests on it, so a non-volatile target
   * means this is not the construct we think it is; javac is content either way.
   */
  public static class NotVolatile {
    Object next;

    static final AtomicReferenceFieldUpdater<NotVolatile, Object> U =
        AtomicReferenceFieldUpdater.newUpdater(NotVolatile.class, Object.class, "next");
  }

  /**
   * A blank final definitely-assigned on both arms of a branch, naming a different field on each.
   * Either write is individually recognizable, which is the trap: substituting one field's offset
   * would leave the other path writing at the wrong address.
   */
  public static class TwoPaths {
    volatile Object a;
    volatile Object b;
    static boolean flag;

    static final AtomicReferenceFieldUpdater<TwoPaths, Object> U;

    static {
      if (flag) {
        U = AtomicReferenceFieldUpdater.newUpdater(TwoPaths.class, Object.class, "a");
      } else {
        U = AtomicReferenceFieldUpdater.newUpdater(TwoPaths.class, Object.class, "b");
      }
    }
  }
}
