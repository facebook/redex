/**
 * Copyright (c) Facebook, Inc. and its affiliates.
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

namespace mog = method_override_graph;

struct MethodOverrideGraphTest : public RedexTest {};

std::string get_overriding_methods(const mog::Graph& graph,
                                   const DexMethodRef* mref) {
  std::vector<std::string> overrides;
  always_assert(mref->is_def());
  auto method = static_cast<const DexMethod*>(mref);
  for (auto* method : mog::get_overriding_methods(graph, method)) {
    overrides.emplace_back(show(method));
  }
  std::sort(overrides.begin(), overrides.end());
  return boost::algorithm::join(overrides, ", ");
}

TEST_F(MethodOverrideGraphTest, verify) {
  const char* dexfile = std::getenv("dexfile");

  std::vector<DexStore> stores;
  DexMetadata dm;
  dm.set_id("classes");
  DexStore root_store(dm);
  root_store.add_classes(load_classes_from_dex(dexfile));
  DexClasses& classes = root_store.get_dexen().back();
  stores.emplace_back(std::move(root_store));

  auto graph = mog::build_graph(build_class_scope(stores));
  EXPECT_EQ(
      get_overriding_methods(
          *graph, DexMethod::get_method("Lcom/facebook/redextest/A;.m:()V")),
      "Lcom/facebook/redextest/B;.m:()V");
  EXPECT_EQ(
      get_overriding_methods(
          *graph, DexMethod::get_method("Lcom/facebook/redextest/A;.n:()V")),
      "");
  EXPECT_EQ(
      get_overriding_methods(
          *graph, DexMethod::get_method("Lcom/facebook/redextest/IA;.m:()V")),
      "Lcom/facebook/redextest/A;.m:()V, Lcom/facebook/redextest/B;.m:()V");
  EXPECT_EQ(
      get_overriding_methods(
          *graph, DexMethod::get_method("Lcom/facebook/redextest/IB;.m:()V")),
      "Lcom/facebook/redextest/B;.m:()V");
  EXPECT_EQ(
      get_overriding_methods(
          *graph, DexMethod::get_method("Lcom/facebook/redextest/IC;.m:()V")),
      "Lcom/facebook/redextest/B;.m:()V");
  EXPECT_EQ(
      get_overriding_methods(
          *graph, DexMethod::get_method("Lcom/facebook/redextest/IB;.n:()V")),
      "Lcom/facebook/redextest/A;.n:()V");
}
