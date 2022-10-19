/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "IPReflectionAnalysis.h"
#include "IROpcode.h"
#include "Pass.h"
#include "RedexTest.h"
#include "Show.h"

#include <json/value.h>
#include <string>

struct IPReflectionAnalysisTest : public RedexIntegrationTest {

 protected:
  std::unique_ptr<PassManager> pass_manager;
  std::unique_ptr<IPReflectionAnalysisPass> analysis_pass;

 public:
  void SetUp() override {
    analysis_pass = std::make_unique<IPReflectionAnalysisPass>();
  }

  void run_passes() {
    Json::Value config(Json::objectValue);
    config["redex"] = Json::objectValue;
    config["redex"]["passes"] = Json::arrayValue;
    config["redex"]["passes"].append("IPReflectionAnalysisPass");
    config["IPReflectionAnalysisPass"] = Json::objectValue;
    ConfigFiles conf(config);
    conf.parse_global_config();
    std::vector<Pass*> passes{analysis_pass.get()};
    pass_manager = std::make_unique<PassManager>(passes, conf);
    pass_manager->set_testing_mode();
    pass_manager->run_passes(stores, conf);
  }
};

const DexMethod* extract_method_in_tests(const std::string& name) {
  std::string method_full_name =
      "Lcom/facebook/redextest/IPReflectionAnalysisTest;." + name + ":()V";
  const DexMethod* method = DexMethod::get_method(method_full_name)->as_def();
  return method;
}

TEST_F(IPReflectionAnalysisTest, test_results) {
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

  auto analysis =
      pass_manager->get_preserved_analysis<IPReflectionAnalysisPass>();
  ASSERT_NE(nullptr, analysis);

  auto results = analysis->get_result();
  ASSERT_NE(nullptr, results);

  std::map<std::string, size_t> expected_entries{
      {"Lcom/facebook/redextest/IPReflectionAnalysisTest;.reflClass:()Ljava/"
       "lang/Class;",
       3},
      {"Lcom/facebook/redextest/IPReflectionAnalysisTest;.reflMethod:()Ljava/"
       "lang/reflect/Method;",
       10},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.callsReflMethod:()Ljava/lang/reflect/Method;",
       3},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.callsReflClass:()Ljava/lang/Class;",
       3},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.reflMethodWithCallsReflClass:()Ljava/lang/"
       "reflect/Method;",
       10},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.reflMethodWithInputClass:(Ljava/lang/"
       "Class;)Ljava/lang/reflect/Method;",
       9},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.callsReflMethodWithInputClass:()Ljava/lang/"
       "reflect/Method;",
       5},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.reflClassWithInputString:(Ljava/lang/"
       "String;)Ljava/lang/Class;",
       3},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.callsReflClassWithInputString:()Ljava/lang/"
       "Class;",
       3},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.reflMethodWithInputString:(Ljava/lang/"
       "String;Ljava/lang/String;)Ljava/lang/reflect/Method;",
       8},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.reflClassWithCallGetClassName:()Ljava/lang/"
       "Class;",
       3},
      {"Lcom/facebook/redextest/IPReflectionAnalysisTest;.getClassName:()Ljava/"
       "lang/String;",
       0},
      {"Lcom/facebook/redextest/"
       "IPReflectionAnalysisTest;.callsReflMethodWithInputString:()Ljava/lang/"
       "reflect/Method;",
       3},
      {"Lcom/facebook/redextest/Base;.reflBaseClass:()Ljava/lang/Class;", 3},
      {"Lcom/facebook/redextest/Base;.reflString:(Ljava/lang/String;)Ljava/"
       "lang/Class;",
       0},
      {"Lcom/facebook/redextest/Extended;.reflBaseClass:()Ljava/lang/Class;",
       3},
      {"Lcom/facebook/redextest/Extended;.reflString:(Ljava/lang/String;)Ljava/"
       "lang/Class;",
       3},
      {"Lcom/facebook/redextest/Extended;.callsReflBaseClass:()Ljava/lang/"
       "Class;",
       3},
      {"Lcom/facebook/redextest/Extended;.callsReflString:()Ljava/lang/Class;",
       3},
      {"Lcom/facebook/redextest/ExtendedExtended;.callsReflString:()Ljava/lang/"
       "Class;",
       3},
  };

  for (const auto& entry : expected_entries) {
    DexMethod* method = DexMethod::get_method(entry.first)->as_def();
    auto actual = results->at(method).size();
    EXPECT_EQ(actual, entry.second)
        << "Expected " << entry.second << " entries for method "
        << show(entry.first) << " but " << actual << " were found.";
  }
}
