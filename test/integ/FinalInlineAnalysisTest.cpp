/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>
#include <sstream>

#include "FinalInlineV2.h"
#include "RedexTest.h"

class FinalInlineAnalysisTest : public RedexIntegrationTest {};

TEST_F(FinalInlineAnalysisTest, test_all) {
  auto scope = build_class_scope(stores);
  auto call_graph = final_inline::build_class_init_graph(scope);
  final_inline::StaticFieldReadAnalysis analysis(call_graph, {});

  std::vector<std::pair<std::string, size_t>> expected_entries{
      {"Lcom/facebook/redextest/InitReadsNothing;.<clinit>:()V", 0},
      {"Lcom/facebook/redextest/InitDirectlyReadsOneStaticField;.<clinit>:()V",
       1},
      {"Lcom/facebook/redextest/"
       "InitIndirectlyReadsOneStaticField;.<clinit>:()V",
       1},
      {"Lcom/facebook/redextest/InitInvokesRecursion;.<clinit>:()V", 1},
      {"Lcom/facebook/redextest/InitInvokesMutualRecursion;.<clinit>:()V", 2},
      {"Lcom/facebook/redextest/InitInvokesVirtual;.<clinit>:()V", 1},
      {"Lcom/facebook/redextest/InitInvokesVirtualRecursion;.<clinit>:()V", 2},
  };

  for (const auto& entry : expected_entries) {
    DexMethod* method = DexMethod::get_method(entry.first)->as_def();
    ASSERT_TRUE(method) << entry.first << " not found.";
    auto actual = analysis.analyze(method);
    ASSERT_FALSE(actual.is_bottom())
        << "Result for method " << entry.first << " is bottom.";
    ASSERT_FALSE(actual.is_top())
        << "Result for method " << entry.first << " is top.";
    std::ostringstream oss;
    oss << "{";
    bool is_first = true;
    for (const auto& element : actual.elements()) {
      if (is_first) {
        is_first = false;
      } else {
        oss << ", ";
      }
      oss << show(element);
    }
    oss << "}";
    EXPECT_EQ(actual.elements().size(), entry.second)
        << "Expected " << entry.second << " entries for method " << entry.first
        << " but " << actual.elements().size()
        << " were found. Elements: " << oss.str();
  }
}
