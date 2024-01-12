/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodOverrideGraph.h"

#include <boost/algorithm/string/join.hpp>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "DexLoader.h"
#include "RedexTest.h"
#include "Show.h"

namespace mog = method_override_graph;

struct MethodOverrideGraphTest : public RedexIntegrationTest {};

std::vector<std::string> get_overriding_methods(
    const mog::Graph& graph,
    const DexMethodRef* mref,
    bool include_interface,
    const DexType* parent_class = nullptr) {
  std::vector<std::string> overriding;
  always_assert(mref->is_def());
  auto method = static_cast<const DexMethod*>(mref);
  for (auto* overriding_method : mog::get_overriding_methods(
           graph, method, include_interface, parent_class)) {
    overriding.emplace_back(show(overriding_method));
  }
  return overriding;
}

std::vector<std::string> get_overridden_methods(const mog::Graph& graph,
                                                const DexMethodRef* mref,
                                                bool include_interface) {
  std::vector<std::string> overridden;
  always_assert(mref->is_def());
  auto method = static_cast<const DexMethod*>(mref);
  for (auto* overridden_method :
       mog::get_overridden_methods(graph, method, include_interface)) {
    overridden.emplace_back(show(overridden_method));
  }
  return overridden;
}

TEST_F(MethodOverrideGraphTest, verify) {
  const char* dexfile = std::getenv("dexfile");
  const char* IA_M = "Lcom/facebook/redextest/IA;.m:()V";
  const char* IB_M = "Lcom/facebook/redextest/IB;.m:()V";
  const char* IB_N = "Lcom/facebook/redextest/IB;.n:()V";
  const char* IC_M = "Lcom/facebook/redextest/IC;.m:()V";
  const char* A_M = "Lcom/facebook/redextest/A;.m:()V";
  const char* A_N = "Lcom/facebook/redextest/A;.n:()V";
  const char* B_M = "Lcom/facebook/redextest/B;.m:()V";
  const char* C_M = "Lcom/facebook/redextest/C;.m:()V";
  const char* IB = "Lcom/facebook/redextest/IB;";
  const char* B = "Lcom/facebook/redextest/B;";
  const char* C = "Lcom/facebook/redextest/C;";

  auto graph = mog::build_graph(build_class_scope(stores));
  // Find the methods that override the given methods
  EXPECT_THAT(
      get_overriding_methods(*graph, DexMethod::get_method(IA_M), false),
      ::testing::UnorderedElementsAre(A_M, B_M, C_M));
  EXPECT_THAT(get_overriding_methods(*graph, DexMethod::get_method(IA_M), true),
              ::testing::UnorderedElementsAre(A_M, B_M, C_M, IB_M));
  EXPECT_THAT(
      get_overriding_methods(*graph, DexMethod::get_method(IB_M), false),
      ::testing::UnorderedElementsAre(B_M, C_M));
  EXPECT_THAT(
      get_overriding_methods(*graph, DexMethod::get_method(IC_M), false),
      ::testing::UnorderedElementsAre(B_M));
  EXPECT_THAT(
      get_overriding_methods(*graph, DexMethod::get_method(IB_N), false),
      ::testing::UnorderedElementsAre(A_N));
  EXPECT_THAT(
      get_overriding_methods(
          *graph, DexMethod::get_method(IB_N), false, DexType::get_type(B)),
      ::testing::UnorderedElementsAre(A_N));
  EXPECT_THAT(
      get_overriding_methods(
          *graph, DexMethod::get_method(IB_N), false, DexType::get_type(IB)),
      ::testing::UnorderedElementsAre(A_N));
  EXPECT_THAT(
      get_overriding_methods(
          *graph, DexMethod::get_method(IB_N), false, DexType::get_type(C)),
      ::testing::UnorderedElementsAre(A_N));

  // Find the methods that the given methods override
  EXPECT_THAT(get_overridden_methods(*graph, DexMethod::get_method(A_M), true),
              ::testing::UnorderedElementsAre(IA_M));
  EXPECT_THAT(get_overridden_methods(*graph, DexMethod::get_method(A_N), true),
              ::testing::UnorderedElementsAre(IB_N));
  EXPECT_THAT(get_overridden_methods(*graph, DexMethod::get_method(IA_M), true),
              ::testing::UnorderedElementsAre());
  EXPECT_THAT(get_overridden_methods(*graph, DexMethod::get_method(IB_M), true),
              ::testing::UnorderedElementsAre(IA_M));
  EXPECT_THAT(get_overridden_methods(*graph, DexMethod::get_method(IC_M), true),
              ::testing::UnorderedElementsAre());
  EXPECT_THAT(get_overridden_methods(*graph, DexMethod::get_method(IB_N), true),
              ::testing::UnorderedElementsAre());
  EXPECT_THAT(get_overridden_methods(*graph, DexMethod::get_method(B_M), false),
              ::testing::UnorderedElementsAre(A_M));
  EXPECT_THAT(get_overridden_methods(*graph, DexMethod::get_method(B_M), true),
              ::testing::UnorderedElementsAre(A_M, IA_M, IB_M, IC_M));

  // Check that parents and children do not contain duplicates
  for (auto&& [method, node] : graph->nodes()) {
    std::unordered_set<const DexMethod*> children(node.children.begin(),
                                                  node.children.end());
    EXPECT_EQ(node.children.size(), children.size());
    std::unordered_set<const DexMethod*> parents(node.parents.begin(),
                                                 node.parents.end());
    EXPECT_EQ(node.parents.size(), parents.size());
  }
}
