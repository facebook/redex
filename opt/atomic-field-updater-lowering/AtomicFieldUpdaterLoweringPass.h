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
 * All three flavors are lowered, across `get`, `set`, `lazySet`,
 * `compareAndSet`, `weakCompareAndSet` and -- for the reference flavor --
 * `getAndSet`. Two things narrow that:
 *
 *   - `getAndSet` and `getAndAdd` reached `sun.misc.Unsafe` only in Android N,
 *     so below min_sdk 24 those sites are counted and skipped. In practice this
 *     bucket only ever holds `getAndSetObject`: the numeric members are
 *     withheld by the restriction below, which is checked first, so they are
 *     counted there whatever the min_sdk.
 *   - `Unsafe.getAndAdd{Int,Long}` and `Unsafe.getAndSet{Int,Long}` are
 *     `max-target-r` under Android's non-SDK interface policy: app code that
 *     targets past API 30 cannot link them, and Android 12 and newer refuse
 *     with NoSuchMethodError. So the arithmetic forms over `int` and `long`
 *     (`getAndAdd`, `addAndGet`, `getAndIncrement` and friends) and the numeric
 *     `getAndSet` are recognized but left alone. `getAndSetObject` carries no
 *     such restriction, so the reference flavor keeps its `getAndSet`.
 *
 * That second restriction is a property of the *caller*, not of the member: an
 * updater's own implementation calls the restricted members freely, because
 * libcore is on the bootclasspath and exempt from the policy. Moving the call
 * into app code is what forfeits the exemption, so every member this pass emits
 * must be classified in `atomic_field_updaters::hidden_api_status` first.
 *
 * Each field offset lives on the class that declares the field and is computed
 * in that class's own `<clinit>`; only the `Unsafe` instance is shared. A
 * holder whose non-nullness cannot be proven gets an explicit check that throws
 * ClassCastException, matching what `accessCheck` would have done.
 *
 * Before any of that the pass inlines the synthetic accessors that would
 * otherwise hide an updater from the analysis: Kotlin keeps the updater in a
 * private field and reads it through a generated getter, behind an `access$`
 * bridge when the use is from a nested class, so the receiver of an operation
 * is defined by an invoke rather than by the field load resolution looks for.
 *
 * The supported shape is intentionally the statically obvious updater-field
 * pattern coroutines and atomicfu emit: a `static final Atomic*FieldUpdater`
 * field on the class that declares the volatile field it updates, optionally
 * hidden behind Kotlin synthetic accessor chains. Helper objects that keep the
 * updater on some other class, like the AbstractFuture-style safe-helper
 * pattern, are outside this pass's scope and are left alone.
 *
 * A receiver the pass cannot trace back to one of those static updater fields
 * -- one passed in as a parameter, or read from a helper object -- has no
 * offset to substitute and is left alone.
 *
 */
class AtomicFieldUpdaterLoweringPass : public Pass {
 public:
  AtomicFieldUpdaterLoweringPass() : Pass("AtomicFieldUpdaterLoweringPass") {}

  void run_pass(DexStoresVector& stores,
                ConfigFiles& conf,
                PassManager& mgr) override;
};
