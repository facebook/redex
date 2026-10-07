/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "AtomicFieldUpdaterLoweringPass.h"
#include "RedexTest.h"

// Accessor selection and inlining against real kotlinc output. This is the one
// place the pass is checked on the bytecode it was written for: the whole diff
// exists because Kotlin puts a synthetic getter, and an `access$` bridge, in
// front of a private updater field. Asserting that on hand-assembled IR only
// establishes that the pass handles what we imagined kotlinc emits.
class AtomicFieldUpdaterAccessorsTest : public RedexIntegrationTest {
 protected:
  int64_t metric(const std::string& key) {
    for (const auto& info : pass_manager->get_pass_info()) {
      if (info.name.find("AtomicFieldUpdaterLowering") == std::string::npos) {
        continue;
      }
      auto it = info.metrics.find(key);
      if (it != info.metrics.end()) {
        return it->second;
      }
    }
    return -1;
  }

  void run() {
    std::vector<Pass*> passes{new AtomicFieldUpdaterLoweringPass()};
    run_passes(passes);
  }
};

TEST_F(AtomicFieldUpdaterAccessorsTest, resolvesThroughKotlinAccessorChain) {
  run();
  EXPECT_EQ(metric("updaters_recognized"), 2);
  // kotlinc emits one accessor per holder for this shape: `access$getU$cp()` on
  // the class declaring the backing field. Pinned rather than asserted
  // non-zero, so a codegen change that adds or removes a level is visible here.
  EXPECT_EQ(metric("accessors_selected"), 2);
  EXPECT_EQ(metric("accessors_inlined"), metric("accessors_selected"));
  EXPECT_EQ(metric("accessors_rejected_impure"), 0);
  // Every call site resolves once the chains are flattened: two on the first
  // holder, one compareAndSet on the second.
  EXPECT_EQ(metric("rewritable_total"), 3);
}

// Inlining copies an accessor's body without removing the original, whose
// leftover `sget-object` would go on referencing the updater and keep cleanup
// from ever removing the field. Deleting the accessors is what lets this holder
// shed its updater, and only the Kotlin shape needs it: read directly, as in
// hand-written Java, there is no accessor in the way.
TEST_F(AtomicFieldUpdaterAccessorsTest, inlinedAccessorsAreDeleted) {
  run();
  EXPECT_EQ(metric("accessors_deleted"), 2);
  EXPECT_EQ(metric("updater_fields_removed"), 2);
}

// The coroutines shape: a Kotlin accessor chain in front of the updater, and
// d8's forwarder in front of the compareAndSet. The second holder's updater is
// used only through compareAndSet, so its accessor is reachable only from the
// forwarder call -- an accessor search that starts from updater calls alone
// would never find it, and the site would stay behind both wrappers.
TEST_F(AtomicFieldUpdaterAccessorsTest, lowersCompareAndSetBehindBothWrappers) {
  run();
  EXPECT_EQ(metric("ops_backport_cas_calls"), 1);
  EXPECT_EQ(metric("backport_cas_calls_rewritten"), 1);
  EXPECT_EQ(metric("cas_retry_calls_emitted"), 1);
}
