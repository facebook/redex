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

class AtomicFieldUpdaterLoweringIntegTest : public RedexIntegrationTest {
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

// The three flavors are recognized from real javac output, where `newUpdater`
// is called in <clinit> exactly as an app would write it. The unit tests
// assemble that shape by hand and so cannot show it survives a real compile.
TEST_F(AtomicFieldUpdaterLoweringIntegTest, recognizesEveryFlavor) {
  run();
  EXPECT_EQ(metric("updaters_recognized"), 3);
  EXPECT_EQ(metric("updaters_recognized_reference"), 1);
  EXPECT_EQ(metric("updaters_recognized_integer"), 1);
  EXPECT_EQ(metric("updaters_recognized_long"), 1);

  // The census counts every call against an updater type, including the two
  // inside the forwarder D8 synthesizes for the reference compareAndSet.
  EXPECT_EQ(metric("ops_total"), 11);
}
