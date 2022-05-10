/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "AnalysisUsage.h"
#include "DexClass.h"
#include "MaxDepthAnalysis.h"
#include "Pass.h"
#include "RedexTest.h"

#include <json/value.h>
#include <string>

class AnalysisConsumerPass : public Pass {
 public:
  AnalysisConsumerPass() : Pass("AnalysisConsumerPass") {}
  void set_analysis_usage(AnalysisUsage& au) const override {
    au.add_required<MaxDepthAnalysisPass>();
    au.set_preserve_all();
  }

  void run_pass(DexStoresVector& /* stores */,
                ConfigFiles& /* conf */,
                PassManager& mgr) override {
    auto preserved = mgr.get_preserved_analysis<MaxDepthAnalysisPass>();
    always_assert(preserved);
    auto result = preserved->get_result();
    always_assert(!result->empty());
  }
};

struct MaxDepthAnalysisTest : public RedexIntegrationTest {

 protected:
  std::unique_ptr<PassManager> pass_manager;
  std::unique_ptr<MaxDepthAnalysisPass> analysis_pass;
  std::unique_ptr<AnalysisConsumerPass> consumer_pass;

 public:
  void SetUp() override {
    analysis_pass = std::make_unique<MaxDepthAnalysisPass>();
    consumer_pass = std::make_unique<AnalysisConsumerPass>();
  }
  void run_passes() {
    Json::Value config(Json::objectValue);
    config["redex"] = Json::objectValue;
    config["redex"]["passes"] = Json::arrayValue;
    config["redex"]["passes"].append("MaxDepthAnalysisPass");
    config["redex"]["passes"].append("AnalysisConsumerPass");
    config["MaxDepthAnalysisPass"] = Json::objectValue;
    config["AnalysisConsumerPass"] = Json::objectValue;
    ConfigFiles conf(config);
    std::vector<Pass*> passes{analysis_pass.get(), consumer_pass.get()};
    pass_manager = std::make_unique<PassManager>(passes, conf);
    pass_manager->set_testing_mode();
    pass_manager->run_passes(stores, conf);
  }
};

const DexMethod* extract_method_in_tests(const std::string& name) {
  std::string method_full_name =
      "Lcom/facebook/redextest/MaxDepthAnalysisTest;." + name + ":()V";
  const DexMethod* method = DexMethod::get_method(method_full_name)->as_def();
  return method;
}

TEST_F(MaxDepthAnalysisTest, test_results) {
  auto scope = build_class_scope(stores);

  // otherwise call graph won't include the calls
  for (auto cls : scope) {
    for (auto m : cls->get_dmethods()) {
      m->rstate.set_root();
    }

    for (auto m : cls->get_vmethods()) {
      m->rstate.set_root();
    }
  }

  run_passes();

  // The preserved analysis should still exist because AnalysisConsumerPass is
  // set to preserve all analyses.
  MaxDepthAnalysisPass* preserved =
      pass_manager->get_preserved_analysis<MaxDepthAnalysisPass>();
  ASSERT_NE(nullptr, preserved);

  auto results = preserved->get_result();

  constexpr int total_functions = 9;

  for (int i = 0; i < total_functions; i++) {
    const auto method = extract_method_in_tests("a" + std::to_string(i));
    ASSERT_NE(nullptr, method);
    auto actual = results->at(method);
    EXPECT_EQ(actual, i) << "Method a" << i
                         << " result was wrong: " << results->at(method);
  }

  // Functions recursive1 and recursive2 are mutually recursive. No result
  // should exist for them.
  const auto method_recursive1 = extract_method_in_tests("recursive1");
  ASSERT_NE(nullptr, method_recursive1);
  EXPECT_EQ(0, results->count(method_recursive1));

  const auto method_recursive2 = extract_method_in_tests("recursive2");
  ASSERT_NE(nullptr, method_recursive2);
  EXPECT_EQ(0, results->count(method_recursive2));
}
