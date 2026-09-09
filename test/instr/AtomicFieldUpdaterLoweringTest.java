/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.redex.test.instr;

import static org.assertj.core.api.Assertions.*;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicIntegerFieldUpdater;
import java.util.concurrent.atomic.AtomicLongFieldUpdater;
import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;
import org.junit.Before;
import org.junit.Test;

/**
 * Runs the operations AtomicFieldUpdaterLoweringPass rewrites, on device, after they have been
 * rewritten.
 *
 * <p>The pass replaces the updater's reflective accessCheck/valueCheck with a direct sun.misc.Unsafe
 * memory operation. Unsafe has no safety net: a wrong offset, a wrong register width or a dropped
 * null check is silent memory corruption rather than an exception, and none of that is visible to a
 * unit test asserting on IR. These assertions only hold if the rewritten code is actually correct at
 * runtime.
 */
public class AtomicFieldUpdaterLoweringTest {

  // The atomicfu shape: a volatile field plus a static final updater over it.
  static class Holder {
    volatile Object ref = "initial";
    volatile int i = 0;
    volatile long l = 0L;

    static final AtomicReferenceFieldUpdater<Holder, Object> REF =
        AtomicReferenceFieldUpdater.newUpdater(Holder.class, Object.class, "ref");
    static final AtomicIntegerFieldUpdater<Holder> I =
        AtomicIntegerFieldUpdater.newUpdater(Holder.class, "i");
    static final AtomicLongFieldUpdater<Holder> L =
        AtomicLongFieldUpdater.newUpdater(Holder.class, "l");
  }

  static class OtherHolder {
    volatile Object ref = "other";
  }

  static class StringHolder {
    volatile String value = "initial";

    static final AtomicReferenceFieldUpdater<StringHolder, String> REF =
        AtomicReferenceFieldUpdater.newUpdater(StringHolder.class, String.class, "value");
  }

  private Holder h;

  /**
   * A weak CAS may fail spuriously, so a single attempt proves nothing; an unbounded retry would
   * hang if the lowering made success impossible. This caps the retries so that case fails as an
   * assertion naming the operation instead of as a harness timeout.
   */
  private static final int MAX_CAS_ATTEMPTS = 1000;

  private interface WeakCas {
    boolean attempt();
  }

  private static boolean retryWeakCas(WeakCas cas) {
    for (int i = 0; i < MAX_CAS_ATTEMPTS; i++) {
      if (cas.attempt()) {
        return true;
      }
    }
    return false;
  }

  @Before
  public void setup() {
    h = new Holder();
  }

  @Test
  public void referenceGetSet() {
    assertThat(Holder.REF.get(h)).isEqualTo("initial");
    Holder.REF.set(h, "updated");
    assertThat(Holder.REF.get(h)).isEqualTo("updated");
    // The rewrite must not decouple the updater from the field it aliases.
    assertThat(h.ref).isEqualTo("updated");
  }

  @Test
  public void referenceCompareAndSet() {
    // Failing CAS must leave the field alone and report false.
    assertThat(Holder.REF.compareAndSet(h, "wrong", "new")).isFalse();
    assertThat(Holder.REF.get(h)).isEqualTo("initial");

    assertThat(Holder.REF.compareAndSet(h, "initial", "new")).isTrue();
    assertThat(Holder.REF.get(h)).isEqualTo("new");
  }

  @Test
  public void referenceGetAndSetAndLazySet() {
    assertThat(Holder.REF.getAndSet(h, "second")).isEqualTo("initial");
    assertThat(Holder.REF.get(h)).isEqualTo("second");

    Holder.REF.lazySet(h, "third");
    assertThat(Holder.REF.get(h)).isEqualTo("third");
  }

  @Test
  public void referenceNullValueIsAllowed() {
    // valueCheck permits null; the rewrite must not reject or mangle it.
    Holder.REF.set(h, null);
    assertThat(Holder.REF.get(h)).isNull();
    assertThat(Holder.REF.compareAndSet(h, null, "back")).isTrue();
    assertThat(Holder.REF.get(h)).isEqualTo("back");
  }

  @Test
  public void integerOperations() {
    assertThat(Holder.I.get(h)).isEqualTo(0);
    Holder.I.set(h, 5);
    assertThat(Holder.I.get(h)).isEqualTo(5);

    assertThat(Holder.I.compareAndSet(h, 4, 99)).isFalse();
    assertThat(Holder.I.compareAndSet(h, 5, 10)).isTrue();
    assertThat(Holder.I.get(h)).isEqualTo(10);

    // getAndAdd returns the OLD value; the *AndGet forms return the NEW one.
    // The rewrite implements the latter as the former plus a fixup, so these
    // catch a fixup applied in the wrong direction or omitted.
    assertThat(Holder.I.getAndIncrement(h)).isEqualTo(10);
    assertThat(Holder.I.get(h)).isEqualTo(11);
    assertThat(Holder.I.incrementAndGet(h)).isEqualTo(12);
    assertThat(Holder.I.getAndDecrement(h)).isEqualTo(12);
    assertThat(Holder.I.decrementAndGet(h)).isEqualTo(10);
    assertThat(Holder.I.getAndAdd(h, 5)).isEqualTo(10);
    assertThat(Holder.I.addAndGet(h, 5)).isEqualTo(20);
    assertThat(Holder.I.getAndSet(h, 42)).isEqualTo(20);
    assertThat(Holder.I.get(h)).isEqualTo(42);
  }

  @Test
  public void longOperationsAcrossWordBoundary() {
    // Values above 2^32 exercise the wide register pair. A rewrite that used a
    // narrow temporary or the wrong move-result form would truncate here while
    // passing every small-value assertion.
    final long big = 0x1234_5678_9ABCL;
    Holder.L.set(h, big);
    assertThat(Holder.L.get(h)).isEqualTo(big);

    assertThat(Holder.L.compareAndSet(h, big - 1, 0L)).isFalse();
    assertThat(Holder.L.compareAndSet(h, big, big + 1)).isTrue();
    assertThat(Holder.L.get(h)).isEqualTo(big + 1);

    assertThat(Holder.L.getAndIncrement(h)).isEqualTo(big + 1);
    assertThat(Holder.L.incrementAndGet(h)).isEqualTo(big + 3);
    assertThat(Holder.L.getAndAdd(h, 0x1_0000_0000L)).isEqualTo(big + 3);
    assertThat(Holder.L.get(h)).isEqualTo(big + 3 + 0x1_0000_0000L);
    assertThat(Holder.L.addAndGet(h, -0x1_0000_0000L)).isEqualTo(big + 3);
    assertThat(Holder.L.getAndSet(h, Long.MIN_VALUE)).isEqualTo(big + 3);
    assertThat(Holder.L.get(h)).isEqualTo(Long.MIN_VALUE);
  }

  @Test
  public void primitiveLazySetAndWeakCompareAndSet() {
    // The only two Unsafe members the pass can emit that no other test here
    // executes: lazySet on the primitive flavors lowers to putOrderedInt and
    // putOrderedLong. An ordered store is a weaker write than a volatile one,
    // so getting the width or the ordering wrong produces a value that is
    // simply never observed -- nothing an IR-level assertion can see.
    Holder.I.lazySet(h, 11);
    assertThat(Holder.I.get(h)).isEqualTo(11);

    final long big = 0x2_0000_0003L;
    Holder.L.lazySet(h, big);
    assertThat(Holder.L.get(h)).isEqualTo(big);

    // weakCompareAndSet lowers to the same compareAndSwap* as compareAndSet, so
    // this pins the mapping rather than the member. Retried because the weak
    // form is permitted to fail spuriously; lowering it to the strong form is
    // sound precisely because a caller must already tolerate that.
    //
    // Bounded rather than a bare `while`: a lowering that compared the wrong
    // offset or width would never succeed, and an unbounded loop would report
    // that as a harness timeout instead of naming the broken operation.
    assertThat(retryWeakCas(() -> Holder.I.weakCompareAndSet(h, 11, 12)))
        .as("integer weakCompareAndSet never succeeded in %d attempts", MAX_CAS_ATTEMPTS)
        .isTrue();
    assertThat(Holder.I.get(h)).isEqualTo(12);

    assertThat(retryWeakCas(() -> Holder.L.weakCompareAndSet(h, big, big + 1)))
        .as("long weakCompareAndSet never succeeded in %d attempts", MAX_CAS_ATTEMPTS)
        .isTrue();
    assertThat(Holder.L.get(h)).isEqualTo(big + 1);
  }

  @Test
  public void nullHolderThrowsClassCastException() {
    // accessCheck calls cclass.isInstance(obj), which is false for null, so a
    // null holder throws ClassCastException -- NOT NullPointerException. Where
    // the pass cannot prove non-nullness it emits a guard to preserve exactly
    // this, so getting it wrong shows up here.
    Holder nullHolder = null;

    assertThatThrownBy(() -> Holder.REF.get(nullHolder))
        .isInstanceOf(ClassCastException.class);

    assertThatThrownBy(() -> Holder.REF.compareAndSet(nullHolder, "a", "b"))
        .isInstanceOf(ClassCastException.class);

    assertThatThrownBy(() -> Holder.I.incrementAndGet(nullHolder))
        .isInstanceOf(ClassCastException.class);
  }

  @Test
  public void wrongHolderTypeThrowsClassCastException() {
    AtomicReferenceFieldUpdater raw = Holder.REF;

    assertThatThrownBy(() -> raw.get(new OtherHolder())).isInstanceOf(ClassCastException.class);
    assertThatThrownBy(() -> raw.compareAndSet(new OtherHolder(), "a", "b"))
        .isInstanceOf(ClassCastException.class);
  }

  @Test
  public void wrongValueTypeThrowsClassCastException() {
    AtomicReferenceFieldUpdater raw = StringHolder.REF;
    StringHolder stringHolder = new StringHolder();

    assertThatThrownBy(() -> raw.set(stringHolder, new Object()))
        .isInstanceOf(ClassCastException.class);
    assertThat(StringHolder.REF.get(stringHolder)).isEqualTo("initial");
  }

  @Test
  public void updatersOnDistinctFieldsDoNotAlias() {
    // Each updater resolves to its own field offset. Reusing one offset for
    // another field is the failure mode a single-field test cannot see.
    Holder.REF.set(h, "r");
    Holder.I.set(h, 7);
    Holder.L.set(h, 9L);

    assertThat(Holder.REF.get(h)).isEqualTo("r");
    assertThat(Holder.I.get(h)).isEqualTo(7);
    assertThat(Holder.L.get(h)).isEqualTo(9L);
    assertThat(h.ref).isEqualTo("r");
    assertThat(h.i).isEqualTo(7);
    assertThat(h.l).isEqualTo(9L);
  }

  @Test
  public void distinctInstancesDoNotShareState() {
    // The offset is per-field, not per-object; applying it to the wrong
    // receiver would show up as two instances aliasing.
    Holder other = new Holder();
    Holder.I.set(h, 1);
    Holder.I.set(other, 2);
    assertThat(Holder.I.get(h)).isEqualTo(1);
    assertThat(Holder.I.get(other)).isEqualTo(2);
  }

  @Test
  public void casLoopUnderContention() throws Exception {
    // The shape coroutines actually use: a CAS retry loop. Runs it from several
    // threads so a rewrite that broke atomicity (a non-volatile access, say)
    // loses updates rather than merely returning a wrong value once.
    final int threads = 4;
    final int perThread = 2000;
    // Every thread blocks on the same latch, so they contend from the first
    // iteration. Starting each thread as it is created would let the earliest
    // ones run to completion before the last is even started, and the assertion
    // below would then hold without the threads ever having raced.
    final CountDownLatch start = new CountDownLatch(1);
    Thread[] ts = new Thread[threads];
    for (int t = 0; t < threads; t++) {
      ts[t] =
          new Thread(
              () -> {
                try {
                  start.await();
                } catch (InterruptedException e) {
                  Thread.currentThread().interrupt();
                  return;
                }
                for (int n = 0; n < perThread; n++) {
                  while (true) {
                    int cur = Holder.I.get(h);
                    if (Holder.I.compareAndSet(h, cur, cur + 1)) {
                      break;
                    }
                  }
                }
              });
      ts[t].start();
    }
    start.countDown();
    for (Thread t : ts) {
      t.join();
    }
    assertThat(Holder.I.get(h)).isEqualTo(threads * perThread);
  }
}
