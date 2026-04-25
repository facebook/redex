/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <gtest/gtest.h>
#include <json/json.h> // NOLINT(facebook-unused-include-check)

#include "ClinitBatchingPass.h"
#include "DexClass.h"
#include "MethodUtil.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"
#include "Walkers.h"

class ClinitBatchingTest : public RedexIntegrationTest {
 public:
  void run_test(const std::string& method_profile_path) {
    auto* config_file_env = std::getenv("config_file");
    always_assert_log(config_file_env,
                      "Config file must be specified to ClinitBatchingTest.\n");

    std::ifstream config_file(config_file_env, std::ifstream::binary);
    Json::Value cfg;
    config_file >> cfg;

    Json::Value mp_val = Json::arrayValue;
    mp_val.resize(1);
    mp_val[0] = method_profile_path;
    cfg["agg_method_stats_files"] = mp_val;

    setup_apk_dir(cfg);

    Pass* pass = new ClinitBatchingPass();
    std::vector<Pass*> passes = {pass};

    run_passes(passes, nullptr, cfg);
  }

  void setup_apk_dir(Json::Value& cfg) {
    m_apk_dir = redex::make_tmp_dir("clinit_batching_test_%%%%%%%%");
    auto* manifest_path_env = std::getenv("manifest");
    always_assert_log(manifest_path_env,
                      "manifest env var must be set for ClinitBatchingTest.\n");
    redex::copy_file(manifest_path_env,
                     m_apk_dir.path + "/AndroidManifest.xml");
    cfg["apk_dir"] = m_apk_dir.path;
  }

  DexMethod* find_clinit(const std::string& class_name) {
    auto* type = DexType::get_type(class_name);
    if (type == nullptr) {
      return nullptr;
    }
    auto* cls = type_class(type);
    if (cls == nullptr) {
      return nullptr;
    }
    for (auto* m : cls->get_dmethods()) {
      if (method::is_clinit(m)) {
        return m;
      }
    }
    return nullptr;
  }

  size_t count_clinits() {
    size_t count = 0;
    auto scope = build_class_scope(stores);
    walk::methods(scope, [&count](DexMethod* m) {
      if (method::is_clinit(m)) {
        count++;
      }
    });
    return count;
  }

 private:
  redex::TempDir m_apk_dir;
};

/**
 * Test that the pass identifies hot clinits as candidates based on
 * method profile data.
 *
 * This test verifies that:
 * 1. Clinits marked as hot in the method profile are identified as candidates
 * 2. The candidate_clinits_count metric reflects the correct number
 */
TEST_F(ClinitBatchingTest, test_candidate_selection_with_profiles) {
  auto* method_profile_path = std::getenv("method-profile");
  ASSERT_NE(method_profile_path, nullptr) << "Missing method-profile path.";

  // Verify we have clinits in the test classes before running the pass
  auto* clinit_a = find_clinit("Lcom/facebook/redextest/ClinitBatchingTest;");
  auto* clinit_b = find_clinit("Lcom/facebook/redextest/ClinitBatchingTestB;");
  auto* clinit_c = find_clinit("Lcom/facebook/redextest/ClinitBatchingTestC;");
  auto* clinit_small =
      find_clinit("Lcom/facebook/redextest/ClinitBatchingTestSmall;");

  ASSERT_NE(clinit_a, nullptr) << "ClinitBatchingTest should have a clinit";
  ASSERT_NE(clinit_b, nullptr) << "ClinitBatchingTestB should have a clinit";
  ASSERT_NE(clinit_c, nullptr) << "ClinitBatchingTestC should have a clinit";
  ASSERT_NE(clinit_small, nullptr)
      << "ClinitBatchingTestSmall should have a clinit";

  run_test(method_profile_path);

  // Verify the pass ran and found candidates
  const auto& metrics = pass_manager->get_pass_info()[0].metrics;

  // We should have 3 candidates (A, B, C) since they are marked as hot
  auto candidate_count_it = metrics.find("candidate_clinits_count");
  ASSERT_NE(candidate_count_it, metrics.end())
      << "candidate_clinits_count metric should exist";
  EXPECT_EQ(candidate_count_it->second, 3)
      << "Should identify 3 hot clinits as candidates";

  // Verify batched_clinits metric - all 3 candidates use constant
  // assignments to their own fields (SPUT of int constants), so they all
  // pass the safety analysis.
  auto batched_count_it = metrics.find("batched_clinits");
  ASSERT_NE(batched_count_it, metrics.end())
      << "batched_clinits metric should exist";
  EXPECT_EQ(batched_count_it->second, 3)
      << "Should batch 3 clinits that pass safety analysis";

  // Verify exclusion metrics exist
  auto excluded_not_hot_it = metrics.find("excluded_not_hot");
  ASSERT_NE(excluded_not_hot_it, metrics.end())
      << "excluded_not_hot metric should exist";
  // The small class's clinit is not in the profile, so it should be excluded
  // as "not hot"
  EXPECT_GE(excluded_not_hot_it->second, 1)
      << "At least the small clinit should be excluded as not hot";
}

/**
 * Test that without method profiles, no candidates are identified.
 * This verifies the pass doesn't do anything when profile data is missing.
 */
TEST_F(ClinitBatchingTest, test_no_candidates_without_profiles) {
  auto* config_file_env = std::getenv("config_file");
  ASSERT_NE(config_file_env, nullptr) << "Missing config_file path.";

  std::ifstream config_file(config_file_env, std::ifstream::binary);
  Json::Value cfg;
  config_file >> cfg;

  // Don't set agg_method_stats_files - no profiles available
  setup_apk_dir(cfg);

  Pass* pass = new ClinitBatchingPass();
  std::vector<Pass*> passes = {pass};

  run_passes(passes, nullptr, cfg);

  // Verify the pass ran but found no candidates
  const auto& metrics = pass_manager->get_pass_info()[0].metrics;

  auto candidate_count_it = metrics.find("candidate_clinits_count");
  ASSERT_NE(candidate_count_it, metrics.end())
      << "candidate_clinits_count metric should exist";
  EXPECT_EQ(candidate_count_it->second, 0)
      << "Should identify 0 candidates without profiles";

  // All 4 test clinits should be excluded as "not hot"
  auto excluded_not_hot_it = metrics.find("excluded_not_hot");
  ASSERT_NE(excluded_not_hot_it, metrics.end())
      << "excluded_not_hot metric should exist";
  EXPECT_EQ(excluded_not_hot_it->second, 4)
      << "All 4 test clinits should be excluded as not hot";
}
