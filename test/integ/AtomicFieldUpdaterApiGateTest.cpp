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

// Below the API level that has the Unsafe method, the operation is recognized,
// counted, and left alone. The other side of the boundary belongs to the
// instrumentation test, which builds at min_sdk 24 and checks on a device that
// the rewrite is not merely emitted but correct.
class AtomicFieldUpdaterApiGateTest : public RedexIntegrationTest {
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
};

TEST_F(AtomicFieldUpdaterApiGateTest, apiGatedOpIsNotRewrittenBelowApi24) {
  RedexOptions options;
  options.min_sdk = 23;
  std::vector<Pass*> passes{new AtomicFieldUpdaterLoweringPass()};
  run_passes(passes, nullptr, Json::nullValue, options);

  EXPECT_EQ(metric("updaters_recognized"), 1) << "found, just not lowerable";
  EXPECT_EQ(metric("ops_total"), 1);
  EXPECT_EQ(metric("blocked_min_sdk"), 1);
  EXPECT_EQ(metric("rewritable_total"), 0);
  EXPECT_EQ(metric("calls_rewritten"), 0);
}
