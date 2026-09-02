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

// Which accessors selection accepts, and how the rejections are counted.
class AtomicFieldUpdaterImpureAccessorTest : public RedexIntegrationTest {
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

// The getter and the bridge above it are both taken; the method with the side
// effect is not, however much it looks like the others from its signature.
TEST_F(AtomicFieldUpdaterImpureAccessorTest, selectsThePureChainOnly) {
  run();
  EXPECT_EQ(metric("accessors_selected"), 2);
  EXPECT_EQ(metric("accessors_inlined"), 2);
}

// `logAndGetU` is reached by following the receiver of the call site in
// `throughImpure`, and rejected there. Counted per distinct callee rather than
// per receiver, so a body reached from several call sites still counts once.
TEST_F(AtomicFieldUpdaterImpureAccessorTest,
       impureAccessorCountedOncePerMethod) {
  run();
  EXPECT_EQ(metric("accessors_rejected_impure"), 1);
}
