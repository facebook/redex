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

// Recognition refusals, checked against real javac output rather than
// assembled IR: what these assert is that shapes a compiler actually produces
// are turned away, and hand-written bytecode cannot establish that.
//
// The fixture deliberately contains no recognizable updater, so the metric
// below is a statement about the whole program and stays meaningful as cases
// are added.
class AtomicFieldUpdaterRejectsTest : public RedexIntegrationTest {
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

// `NotVolatile` names a plain field; `TwoPaths` writes its updater on both arms
// of a branch. Neither may be recognized -- the first is not the construct the
// pass models, and the second has no single field whose offset could stand in
// for the updater at every call site.
TEST_F(AtomicFieldUpdaterRejectsTest, refusesUnrecognizableShapes) {
  run();
  EXPECT_EQ(metric("updaters_recognized"), 0);
  EXPECT_EQ(metric("updaters_recognized_reference"), 0);
}
