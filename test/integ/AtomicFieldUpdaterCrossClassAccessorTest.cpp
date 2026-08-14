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

// An accessor is only followed while every step stays on its own class. This
// fixture is the reject case: a getter that hands back another class's updater
// is pure, and inlining it would preserve semantics, but it is not codegen
// standing in the way and reading its field can run a `<clinit>` that calling
// it did not.
//
// Without this the same-class requirement is unobservable -- every other
// fixture happens to satisfy it, so the check could be deleted and the suite
// would stay green.
class AtomicFieldUpdaterCrossClassAccessorTest : public RedexIntegrationTest {
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

TEST_F(AtomicFieldUpdaterCrossClassAccessorTest, refusesCrossClassAccessor) {
  run();
  // The updater itself is recognized -- what is refused is the getter.
  EXPECT_EQ(metric("updaters_recognized"), 1);
  EXPECT_EQ(metric("accessors_selected"), 0);
  EXPECT_EQ(metric("accessors_inlined"), 0);
  EXPECT_EQ(metric("accessors_rejected_impure"), 1);
  // With the getter left in place the receiver never resolves to a field, so
  // the call site behind it stays out of reach.
  EXPECT_EQ(metric("rewritable_total"), 0);
}
