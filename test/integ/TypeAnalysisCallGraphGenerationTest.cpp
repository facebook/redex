/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "DexClass.h"
#include "GlobalTypeAnalysisPass.h"
#include "TypeAnalysisCallGraphGenerationPass.h"
#include "TypeAnalysisTestBase.h"

using namespace call_graph;
using namespace type_analyzer;
using namespace type_analyzer::global;

class TypeAnalysisCallGraphGenerationTest : public TypeAnalysisTestBase {};

TEST_F(TypeAnalysisCallGraphGenerationTest, Test) {
  auto scope = build_class_scope(stores);
  set_root_method(
      "Lcom/facebook/redextest/TypeAnalysisCallGraphGenerationTest;.main:()V");

  auto gta = new GlobalTypeAnalysisPass();
  auto cggen = new TypeAnalysisCallGraphGenerationPass();
  std::vector<Pass*> passes{gta, cggen};
  run_passes(passes);

  auto cg = cggen->get_result();
  ASSERT_TRUE(cg);

  auto meth_main =
      get_method("TypeAnalysisCallGraphGenerationTest;.main", "", "V");
  ASSERT_TRUE(meth_main);
  ASSERT_TRUE(cg->has_node(meth_main));

  // Check callees of invoke-virtual in meth_main.
  std::unordered_set<const DexMethod*> callees;
  for (auto const& s : GraphInterface::successors(*cg, cg->node(meth_main))) {
    const auto& target = GraphInterface::target(*cg, s);
    callees.insert(target->method());
  }
  ASSERT_TRUE(callees.count(get_method("Base;.getVal", "", "I")));
  ASSERT_TRUE(callees.count(get_method("SubOne;.getVal", "", "I")));
  ASSERT_TRUE(callees.count(get_method("SubTwo;.getVal", "", "I")));

  // TypeAnalysisCallGraphGenerationTest.baseArg() calls SubOne.getVal()
  auto meth_basearg = get_method("TypeAnalysisCallGraphGenerationTest;.baseArg",
                                 "Lcom/facebook/redextest/Base;", "I");
  ASSERT_TRUE(meth_basearg);
  ASSERT_TRUE(cg->has_node(meth_basearg));
  auto successors = GraphInterface::successors(*cg, cg->node(meth_basearg));
  ASSERT_TRUE(successors.size());
  for (auto const& s : successors) {
    const auto& target = GraphInterface::target(*cg, s);
    ASSERT_EQ(target->method(), get_method("SubOne;.getVal", "", "I"));
  }

  // TypeAnalysisCallGraphGenerationTest.intfArg() calls SubOne.getName()
  auto meth_intfarg =
      get_method("TypeAnalysisCallGraphGenerationTest;.intfArg",
                 "Lcom/facebook/redextest/I;", "Ljava/lang/String;");
  ASSERT_TRUE(meth_intfarg);
  ASSERT_TRUE(cg->has_node(meth_intfarg));
  successors = GraphInterface::successors(*cg, cg->node(meth_intfarg));
  ASSERT_TRUE(successors.size());
  for (auto const& s : successors) {
    const auto& target = GraphInterface::target(*cg, s);
    ASSERT_EQ(target->method(),
              get_method("SubOne;.getName", "", "Ljava/lang/String;"));
  }

  // TypeAnalysisCallGraphGenerationTest.baseField() calls SubTwo.getVal()
  auto meth_basefield =
      get_method("TypeAnalysisCallGraphGenerationTest;.baseField", "", "I");
  ASSERT_TRUE(meth_basefield);
  ASSERT_TRUE(cg->has_node(meth_basefield));
  successors = GraphInterface::successors(*cg, cg->node(meth_basefield));
  ASSERT_TRUE(successors.size());
  for (auto const& s : successors) {
    const auto& target = GraphInterface::target(*cg, s);
    ASSERT_EQ(target->method(), get_method("SubTwo;.getVal", "", "I"));
  }

  // TypeAnalysisCallGraphGenerationTest.intfField() calls SubTwo.getName()
  auto meth_intffield =
      get_method("TypeAnalysisCallGraphGenerationTest;.intfField", "",
                 "Ljava/lang/String;");
  ASSERT_TRUE(meth_intffield);
  ASSERT_TRUE(cg->has_node(meth_intffield));
  successors = GraphInterface::successors(*cg, cg->node(meth_intffield));
  ASSERT_TRUE(successors.size());
  for (auto const& s : successors) {
    const auto& target = GraphInterface::target(*cg, s);
    ASSERT_EQ(target->method(),
              get_method("SubTwo;.getName", "", "Ljava/lang/String;"));
  }
}
