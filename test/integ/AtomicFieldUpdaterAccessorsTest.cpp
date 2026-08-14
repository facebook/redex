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
  EXPECT_EQ(metric("updaters_recognized"), 1);
  // kotlinc emits one accessor for this shape: `access$getU$cp()` on the class
  // declaring the backing field. Pinned rather than asserted non-zero, so a
  // codegen change that adds or removes a level is visible here.
  EXPECT_EQ(metric("accessors_selected"), 1);
  EXPECT_EQ(metric("accessors_inlined"), metric("accessors_selected"));
  EXPECT_EQ(metric("accessors_rejected_impure"), 0);
  // Both call sites resolve once the chain is flattened.
  EXPECT_EQ(metric("rewritable_total"), 2);
}
