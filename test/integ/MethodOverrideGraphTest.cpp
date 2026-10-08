/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodOverrideGraph.h"
#include "Debug.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "RedexTest.h"
#include "Show.h"
#include "TypeUtil.h"

namespace mog = method_override_graph;

struct MethodOverrideGraphTest : public RedexIntegrationTest {};

std::vector<std::string> get_overriding_methods(
    const mog::Graph& graph,
    const DexMethodRef* mref,
    bool include_interface,
    const DexType* parent_class = nullptr) {
  std::vector<std::string> overriding;
  always_assert(mref->is_def());
  const auto* method = dynamic_cast<const DexMethod*>(mref);
  auto overriding_methods = mog::get_overriding_methods(
      graph, method, include_interface, parent_class);
  for (const auto* overriding_method : UnorderedIterable(overriding_methods)) {
    overriding.emplace_back(show(overriding_method));
  }
  return overriding;
}

std::vector<std::string> get_overridden_methods(const mog::Graph& graph,
                                                const DexMethodRef* mref,
                                                bool include_interface) {
  std::vector<std::string> overridden;
  always_assert(mref->is_def());
  const auto* method = dynamic_cast<const DexMethod*>(mref);
  auto overridden_methods =
      mog::get_overridden_methods(graph, method, include_interface);
  for (const auto* overridden_method : UnorderedIterable(overridden_methods)) {
    overridden.emplace_back(show(overridden_method));
  }
  return overridden;
}

TEST_F(MethodOverrideGraphTest, verify) {
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
  EXPECT_THAT(get_overriding_methods(*graph, DexMethod::get_method(IB_N), false,
                                     DexType::get_type(B)),
              ::testing::UnorderedElementsAre(A_N));
  EXPECT_THAT(get_overriding_methods(*graph, DexMethod::get_method(IB_N), false,
                                     DexType::get_type(IB)),
              ::testing::UnorderedElementsAre(A_N));
  EXPECT_THAT(get_overriding_methods(*graph, DexMethod::get_method(IB_N), false,
                                     DexType::get_type(C)),
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
  for (auto&& [method, node] : UnorderedIterable(graph->nodes())) {
    std::unordered_set<const mog::Node*> children;
    insert_unordered_iterable(children, node.children);
    EXPECT_EQ(node.children.size(), children.size());
    std::unordered_set<const mog::Node*> parents;
    insert_unordered_iterable(parents, node.parents);
    EXPECT_EQ(node.parents.size(), parents.size());
  }

  // The default graph build is miranda-free.
  EXPECT_FALSE(graph->has_miranda());
}

TEST_F(MethodOverrideGraphTest, miranda_synthesis) {
  const char* ID_P = "Lcom/facebook/redextest/ID;.p:()V";
  const char* AbstractD = "Lcom/facebook/redextest/AbstractD;";
  const char* ConcreteD1_P = "Lcom/facebook/redextest/ConcreteD1;.p:()V";
  const char* ConcreteD2_P = "Lcom/facebook/redextest/ConcreteD2;.p:()V";

  auto cfg = mog::GraphConfig{/*include_miranda=*/true};
  auto graph = mog::build_graph(build_class_scope(stores), cfg);
  EXPECT_TRUE(graph->has_miranda());

  // Sanity: a miranda slot was synthesized at AbstractD for ID.p. Note:
  // miranda is stored as a DexMethodRef cast to DexMethod* — it is NOT a
  // real def (is_def() returns false), only a graph key.
  auto* miranda_ref = DexMethod::get_method(
      DexType::get_type(AbstractD), DexString::get_string("p"),
      DexProto::get_proto(type::_void(), DexTypeList::get_type_list({})));
  ASSERT_NE(miranda_ref, nullptr) << "Expected miranda DexMethodRef to exist";
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  const auto* miranda_as_method = static_cast<const DexMethod*>(miranda_ref);
  const auto& miranda_node = graph->get_node(miranda_as_method);
  EXPECT_TRUE(miranda_node.is_miranda);

  // ID.p should now have the miranda as one of its overriders.
  EXPECT_THAT(
      get_overriding_methods(*graph, DexMethod::get_method(ID_P), false),
      ::testing::IsSupersetOf({ConcreteD1_P, ConcreteD2_P}));

  // find_class_dispatch_root for ConcreteD1.p / ConcreteD2.p resolves to the
  // miranda slot at AbstractD (NOT to ID.p), distinguishing the abstract
  // intermediate as the dispatch root for grouping.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  const auto* d1_p =
      dynamic_cast<const DexMethod*>(DexMethod::get_method(ConcreteD1_P));
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  const auto* d2_p =
      dynamic_cast<const DexMethod*>(DexMethod::get_method(ConcreteD2_P));
  ASSERT_NE(d1_p, nullptr);
  ASSERT_NE(d2_p, nullptr);
  const auto* root_d1 = mog::find_class_dispatch_root(*graph, d1_p);
  const auto* root_d2 = mog::find_class_dispatch_root(*graph, d2_p);
  EXPECT_EQ(root_d1, miranda_ref);
  EXPECT_EQ(root_d2, miranda_ref);

  // Without miranda, find_class_dispatch_root falls back to the original
  // method (no real class root above ConcreteD1.p in non-miranda terms).
  auto plain_graph = mog::build_graph(build_class_scope(stores));
  EXPECT_FALSE(plain_graph->has_miranda());
  EXPECT_EQ(mog::find_class_dispatch_root(*plain_graph, d1_p), d1_p);
}
