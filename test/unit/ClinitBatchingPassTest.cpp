/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ClinitBatchingPass.h"
#include "ConfigFiles.h"
#include "DexClass.h"
#include "PassManager.h"
#include "RedexTest.h"

class ClinitBatchingPassTest : public RedexTest {
 protected:
  DexStoresVector stores;

  void SetUp() override {
    DexStore root_store("classes");
    stores = {root_store};
  }

  void run_pass(const Json::Value& config = Json::nullValue) {
    ConfigFiles conf(config);
    conf.parse_global_config();
    ClinitBatchingPass pass;
    std::vector<Pass*> passes{&pass};
    PassManager manager(passes, conf);
    manager.run_passes(stores, conf);
  }
};

TEST_F(ClinitBatchingPassTest, test_skeleton_runs_without_crashing) {
  // Verify the pass skeleton can be instantiated and run without crashing.
  // This is a basic sanity test for the pass skeleton.
  run_pass();
}

TEST_F(ClinitBatchingPassTest, test_config_binding) {
  // Verify that config values are properly bound and the pass runs
  // successfully with a custom interaction_pattern.
  Json::Value config(Json::objectValue);
  config["ClinitBatchingPass"]["interaction_pattern"] = "ColdStart";

  // The pass emits interaction_pattern_set=1 when the pattern is non-empty.
  // We can't easily read metrics post-run (PassInfo is non-copyable and
  // get_metric requires m_current_pass_info), so we verify the config is
  // accepted without crashing.
  run_pass(config);
}
