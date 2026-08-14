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

// What the classifier does with calls against an updater that it cannot lower.
// Checked against real javac output: `getAndUpdate` compiled from source brings
// a lambda and an invoke-dynamic with it, and the inherited Object methods
// resolve against the updater type the way a compiler actually emits them --
// neither shape is reproduced faithfully by hand-assembled IR.
//
// The fixture holds exactly one recognizable updater and no lowerable
// operation, so the metrics below describe the whole program unambiguously.
class AtomicFieldUpdaterNonOperationsTest : public RedexIntegrationTest {
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

// The updater is recognized, so anything left unrewritten is the classifier's
// doing rather than a failure to find it in the first place.
TEST_F(AtomicFieldUpdaterNonOperationsTest, updaterIsRecognized) {
  run();
  EXPECT_EQ(metric("updaters_recognized"), 1);
}

// `getAndUpdate` counts as an operation -- the census sizes what a lowering
// might reach, not only what this one does -- while `toString` and `equals` are
// not operations at all, merely methods every object inherits. Three calls land
// on the updater and exactly one is counted.
//
// Neither is rewritable: the functional form's written value is produced by the
// operator at runtime. That the pass reaches this assertion at all is part of
// the test -- a zero-argument inherited method read as though its absent first
// argument were the holder is how this analysis crashed before.
TEST_F(AtomicFieldUpdaterNonOperationsTest, onlyRealOperationsAreCounted) {
  run();
  EXPECT_EQ(metric("ops_total"), 1);
  EXPECT_EQ(metric("rewritable_total"), 0);
  EXPECT_EQ(metric("feasible_total"), 0);
}

// With a recognized updater but no site to emit, the shared holder class is
// never created. It exists only to supply the `Unsafe` instance a rewrite
// loads, and its <clinit> reflects over `sun.misc.Unsafe` -- not something to
// put in the primary dex for an app that gets no rewrites out of it.
TEST_F(AtomicFieldUpdaterNonOperationsTest, holderClassIsNotSynthesized) {
  run();
  EXPECT_EQ(metric("calls_rewritten"), 0);
  EXPECT_EQ(type_class(DexType::get_type("Lredex/AtomicFieldUpdaterUnsafe;")),
            nullptr);
}
