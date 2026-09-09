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
  EXPECT_EQ(metric("blocked_hidden_api"), 0)
      << "getAndSetObject is unrestricted; only min_sdk withholds it here";
  EXPECT_EQ(metric("rewritable_total"), 0);
  EXPECT_EQ(metric("calls_rewritten"), 0);
}

// Above the API boundary the same site lowers, which is what makes the
// assertion above about min_sdk rather than about the operation.
TEST_F(AtomicFieldUpdaterApiGateTest, apiGatedOpIsRewrittenAtApi24) {
  RedexOptions options;
  options.min_sdk = 24;
  std::vector<Pass*> passes{new AtomicFieldUpdaterLoweringPass()};
  run_passes(passes, nullptr, Json::nullValue, options);

  EXPECT_EQ(metric("blocked_min_sdk"), 0);
  EXPECT_EQ(metric("blocked_hidden_api"), 0);
  EXPECT_EQ(metric("calls_rewritten"), 1);
}
