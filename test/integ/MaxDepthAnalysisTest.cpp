/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "MaxDepthAnalysis.h"
#include "RedexTest.h"

#include <string>

struct MaxDepthAnalysisTest : public RedexIntegrationTest {};

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

  auto results = max_depth::analyze(scope);

  constexpr int total_functions = 9;

  for (int i = 0; i < total_functions; i++) {
    const auto method = extract_method_in_tests("a" + std::to_string(i));
    ASSERT_NE(nullptr, method);
    auto actual = results.at(method);
    EXPECT_EQ(actual, i) << "Method a" << i
                         << " result was wrong: " << results.at(method);
  }

  // Functions recursive1 and recursive2 are mutually recursive. No result
  // should exist for them.
  const auto method_recursive1 = extract_method_in_tests("recursive1");
  ASSERT_NE(nullptr, method_recursive1);
  EXPECT_EQ(0, results.count(method_recursive1));

  const auto method_recursive2 = extract_method_in_tests("recursive2");
  ASSERT_NE(nullptr, method_recursive2);
  EXPECT_EQ(0, results.count(method_recursive2));
}
