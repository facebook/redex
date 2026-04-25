/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fstream>
#include <gtest/gtest.h>
#include <json/json.h> // NOLINT(facebook-unused-include-check)
#include <optional>

#include "ClinitBatchingPass.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IRCode.h"
#include "MethodUtil.h"
#include "RedexTest.h"
#include "RedexTestUtils.h"
#include "Show.h"
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

  /**
   * Helper to find a method by name in a class.
   */
  DexMethod* find_method(const std::string& class_name,
                         const std::string& method_name) {
    auto* type = DexType::get_type(class_name);
    if (type == nullptr) {
      return nullptr;
    }
    auto* cls = type_class(type);
    if (cls == nullptr) {
      return nullptr;
    }
    for (auto* m : cls->get_dmethods()) {
      if (m->get_name()->str() == method_name) {
        return m;
      }
    }
    return nullptr;
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

  // We should have 6 candidates (Test, TestB, TestC, SimpleClinitClass,
  // DependencyClassA, DependencyClassB) since they are marked as hot
  auto candidate_count_it = metrics.find("candidate_clinits_count");
  ASSERT_NE(candidate_count_it, metrics.end())
      << "candidate_clinits_count metric should exist";
  EXPECT_EQ(candidate_count_it->second, 6)
      << "Should identify 6 hot clinits as candidates";

  // Verify batched_clinits metric - all 6 candidates pass the safety analysis.
  // InitClassesWithSideEffects considers SGET-based clinits (like
  // DependencyClassB which reads DependencyClassA's field) as side-effect-free
  // since the dependency is between candidate classes.
  auto batched_count_it = metrics.find("batched_clinits");
  ASSERT_NE(batched_count_it, metrics.end())
      << "batched_clinits metric should exist";
  EXPECT_EQ(batched_count_it->second, 6)
      << "Should batch 6 clinits that pass safety analysis";

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
  EXPECT_EQ(excluded_not_hot_it->second, 7)
      << "All 4 test clinits should be excluded as not hot";
}

/**
 * Test that the pass actually transforms clinits.
 *
 * This test verifies that:
 * 1. __initStatics$* methods are created for each transformed class
 * 2. Original clinits are removed
 * 3. The transformation metrics are reported
 */
TEST_F(ClinitBatchingTest, test_clinit_transformation) {
  auto* method_profile_path = std::getenv("method-profile");
  ASSERT_NE(method_profile_path, nullptr) << "Missing method-profile path.";

  run_test(method_profile_path);

  // Verify __initStatics$* methods were created
  auto* init_statics_simple =
      find_method("Lcom/facebook/redextest/SimpleClinitClass;",
                  "__initStatics$com_facebook_redextest_SimpleClinitClass");
  auto* init_statics_dep_a =
      find_method("Lcom/facebook/redextest/DependencyClassA;",
                  "__initStatics$com_facebook_redextest_DependencyClassA");

  ASSERT_NE(init_statics_simple, nullptr)
      << "SimpleClinitClass should have __initStatics$* method";
  EXPECT_NE(init_statics_dep_a, nullptr)
      << "DependencyClassA should have __initStatics$* method";

  // Verify original clinits of transformed classes are removed
  auto* clinit_simple =
      find_clinit("Lcom/facebook/redextest/SimpleClinitClass;");
  auto* clinit_dep_a = find_clinit("Lcom/facebook/redextest/DependencyClassA;");

  EXPECT_EQ(clinit_simple, nullptr)
      << "SimpleClinitClass clinit should be removed after transformation";
  EXPECT_EQ(clinit_dep_a, nullptr)
      << "DependencyClassA clinit should be removed after transformation";

  // Verify classes with SGET are transformed (InitClassesWithSideEffects
  // considers them side-effect-free)
  auto* init_statics_dep_b =
      find_method("Lcom/facebook/redextest/DependencyClassB;",
                  "__initStatics$com_facebook_redextest_DependencyClassB");
  EXPECT_NE(init_statics_dep_b, nullptr)
      << "DependencyClassB should have __initStatics$* (SGET accepted)";

  // Verify transformation metrics
  const auto& metrics = pass_manager->get_pass_info()[0].metrics;

  auto transformed_it = metrics.find("transformed_clinits_count");
  ASSERT_NE(transformed_it, metrics.end())
      << "transformed_clinits_count metric should exist";
  EXPECT_EQ(transformed_it->second, 6) << "Should transform 6 clinits";

  // Verify that __initStatics$* methods have code (not empty)
  ASSERT_NE(init_statics_simple->get_code(), nullptr)
      << "__initStatics$* method should have code";
  EXPECT_GT(init_statics_simple->get_code()->estimate_code_units(), 0)
      << "__initStatics$* method should have non-empty code";
}

/**
 * Test that the orchestrator method is filled with invoke-static calls.
 *
 * This test verifies that:
 * 1. The @GenerateStaticInitBatch annotated method is found
 * 2. invoke-static calls are generated for each __initStatics$*() method
 * 3. The orchestrator_generated metric is set to 1
 * 4. The num_init_calls_generated metric equals the number of batched clinits
 * 5. All generated calls target __initStatics$* methods
 */
TEST_F(ClinitBatchingTest, test_orchestrator_generation) {
  auto* method_profile_path = std::getenv("method-profile");
  ASSERT_NE(method_profile_path, nullptr) << "Missing method-profile path.";

  // Find the orchestrator method before running the pass
  auto* orchestrator_type =
      DexType::get_type("Lcom/facebook/redextest/ClinitBatchingOrchestrator;");
  ASSERT_NE(orchestrator_type, nullptr)
      << "ClinitBatchingOrchestrator class should exist";
  auto* orchestrator_cls = type_class(orchestrator_type);
  ASSERT_NE(orchestrator_cls, nullptr)
      << "ClinitBatchingOrchestrator class should be loadable";

  DexMethod* orchestrator_method = nullptr;
  for (auto* m : orchestrator_cls->get_dmethods()) {
    if (m->get_name()->str() == "initAllStatics") {
      orchestrator_method = m;
      break;
    }
  }
  ASSERT_NE(orchestrator_method, nullptr)
      << "initAllStatics method should exist";

  // Verify it has the annotation
  auto* anno_set = orchestrator_method->get_anno_set();
  ASSERT_NE(anno_set, nullptr) << "initAllStatics should have annotations";

  bool has_annotation = false;
  for (const auto& anno : anno_set->get_annotations()) {
    if (anno->type()->str() ==
        "Lcom/facebook/redextest/annotation/GenerateStaticInitBatch;") {
      has_annotation = true;
      break;
    }
  }
  ASSERT_TRUE(has_annotation)
      << "initAllStatics should have @GenerateStaticInitBatch annotation";

  // Count instructions before running the pass
  auto* code_before = orchestrator_method->get_code();
  ASSERT_NE(code_before, nullptr) << "Orchestrator method should have code";
  code_before->build_cfg(/* rebuild */ false);
  size_t insn_count_before = 0;
  for (auto& mie : cfg::InstructionIterable(code_before->cfg())) {
    (void)mie;
    insn_count_before++;
  }
  // Should only have RETURN_VOID initially
  EXPECT_EQ(insn_count_before, 1)
      << "Orchestrator method should be empty (only RETURN_VOID) before pass";

  run_test(method_profile_path);

  // Verify orchestrator metrics
  const auto& metrics = pass_manager->get_pass_info()[0].metrics;

  auto orchestrator_generated_it = metrics.find("orchestrator_generated");
  ASSERT_NE(orchestrator_generated_it, metrics.end())
      << "orchestrator_generated metric should exist";
  EXPECT_EQ(orchestrator_generated_it->second, 1)
      << "orchestrator_generated should be 1";

  auto num_init_calls_it = metrics.find("num_init_calls_generated");
  ASSERT_NE(num_init_calls_it, metrics.end())
      << "num_init_calls_generated metric should exist";
  EXPECT_EQ(num_init_calls_it->second, 6)
      << "Should generate 6 invoke-static calls (one per transformed clinit)";

  // Verify the orchestrator method now has invoke-static calls
  auto* code_after = orchestrator_method->get_code();
  ASSERT_NE(code_after, nullptr)
      << "Orchestrator method should still have code after pass";
  code_after->build_cfg(/* rebuild */ false);

  // Count invoke-static instructions
  size_t invoke_static_count = 0;
  std::vector<DexMethodRef*> called_methods;
  for (auto& mie : cfg::InstructionIterable(code_after->cfg())) {
    if (mie.insn->opcode() == OPCODE_INVOKE_STATIC) {
      invoke_static_count++;
      called_methods.push_back(mie.insn->get_method());
    }
  }

  EXPECT_EQ(invoke_static_count, 6)
      << "Orchestrator should have 6 invoke-static calls";

  // Verify the called methods are __initStatics$* methods
  for (auto* method_ref : called_methods) {
    std::string method_name(method_ref->get_name()->str());
    EXPECT_TRUE(method_name.find("__initStatics$") == 0)
        << "Called method " << method_name
        << " should be an __initStatics$* method";
  }

  // Verify that all 6 expected classes are represented
  if (called_methods.size() == 6) {
    std::optional<size_t> pos_test, pos_testb, pos_testc, pos_simple, pos_dep_a,
        pos_dep_b;
    for (size_t i = 0; i < called_methods.size(); i++) {
      std::string name(called_methods[i]->get_name()->str());
      if (name.find("ClinitBatchingTest;") != std::string::npos ||
          name.find("ClinitBatchingTest$") != std::string::npos ||
          name == "__initStatics$com_facebook_redextest_ClinitBatchingTest") {
        pos_test = i;
      } else if (name.find("ClinitBatchingTestB") != std::string::npos) {
        pos_testb = i;
      } else if (name.find("ClinitBatchingTestC") != std::string::npos) {
        pos_testc = i;
      } else if (name.find("SimpleClinitClass") != std::string::npos) {
        pos_simple = i;
      } else if (name.find("DependencyClassA") != std::string::npos) {
        pos_dep_a = i;
      } else if (name.find("DependencyClassB") != std::string::npos) {
        pos_dep_b = i;
      }
    }
    EXPECT_TRUE(pos_test.has_value())
        << "Should find call to ClinitBatchingTest's init";
    EXPECT_TRUE(pos_testb.has_value())
        << "Should find call to ClinitBatchingTestB's init";
    EXPECT_TRUE(pos_testc.has_value())
        << "Should find call to ClinitBatchingTestC's init";
    EXPECT_TRUE(pos_simple.has_value())
        << "Should find call to SimpleClinitClass's init";
    EXPECT_TRUE(pos_dep_a.has_value())
        << "Should find call to DependencyClassA's init";
    EXPECT_TRUE(pos_dep_b.has_value())
        << "Should find call to DependencyClassB's init";

    // Verify dependency ordering: A must be initialized before B
    if (pos_dep_a.has_value() && pos_dep_b.has_value()) {
      EXPECT_LT(*pos_dep_a, *pos_dep_b)
          << "DependencyClassA should come before DependencyClassB in "
             "dependency order";
    }
  }
}
