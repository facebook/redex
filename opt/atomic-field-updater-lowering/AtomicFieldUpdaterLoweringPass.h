/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Pass.h"

/*
 * AtomicFieldUpdaterLoweringPass lowers `java.util.concurrent.atomic.
 * Atomic{Reference,Integer,Long}FieldUpdater` operations to the
 * `sun.misc.Unsafe` primitives they wrap.
 *
 * An updater exists because Java offers no way to compare-and-swap a field
 * directly: something has to hold the field's offset within the object. The
 * class is constructed from a field *name* at runtime, so it cannot know that
 * callers will pass the right kind of object, and re-verifies on every call:
 *
 *   public final boolean compareAndSet(T obj, V expect, V update) {
 *     accessCheck(obj);    // cclass.isInstance(obj) -> ClassCastException
 *     valueCheck(update);  // vclass.isInstance(update) -> ClassCastException
 *     return U.compareAndSetReference(obj, offset, expect, update);
 *   }
 *
 * Only the last line is work; ART compiles it to a single intrinsic. The rest
 * is a virtual dispatch plus two `Class.isInstance` calls, paid per operation.
 * Where those checks can be discharged at compile time, this pass replaces the
 * call with the primitive alone.
 *
 * The shape it recognizes, which is what `kotlinx.atomicfu` generates and what
 * hand-written lock-free code uses directly:
 *
 *   class Node {
 *     volatile Object next;
 *     static final AtomicReferenceFieldUpdater NEXT =
 *         AtomicReferenceFieldUpdater.newUpdater(Node.class, Object.class,
 *                                                "next");
 *   }
 *
 * This stage recognizes such updaters and reports how many operation call
 * sites exist, per flavor and operation. It does not modify code.
 */
class AtomicFieldUpdaterLoweringPass : public Pass {
 public:
  AtomicFieldUpdaterLoweringPass() : Pass("AtomicFieldUpdaterLoweringPass") {}

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;
};
