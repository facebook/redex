/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ControlFlow.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "LoopInfo.h"
#include "RedexTest.h"
#include "TestLoop.h"
#include "WeakTopologicalOrdering.h"

class LoopInfoTest : public RedexTest {};

class SimpleGraph final {
 public:
  SimpleGraph() {}

  void add_edge(const std::string& source, const std::string& target) {
    m_edges[source].insert(target);
  }

  std::vector<std::string> successors(const std::string& node) {
    auto& succs = m_edges[node];
    return std::vector<std::string>(succs.begin(), succs.end());
  }

 private:
  std::unordered_map<std::string, std::set<std::string>> m_edges;
  friend std::ostream& operator<<(std::ostream&, const SimpleGraph&);
};

TEST_F(LoopInfoTest, visit_depth_first) {
  // 1 2 (3 4 (5 6) 7) 8
  SimpleGraph g;
  g.add_edge("1", "2");
  g.add_edge("2", "3");
  g.add_edge("3", "4");
  g.add_edge("4", "5");
  g.add_edge("5", "6");
  g.add_edge("6", "7");
  g.add_edge("7", "8");
  g.add_edge("2", "8");
  g.add_edge("4", "7");
  g.add_edge("6", "5");
  g.add_edge("7", "3");

  sparta::WeakTopologicalOrdering<std::string> wto(
      "1", [&g](const std::string& n) { return g.successors(n); });

  std::ostringstream s;
  for (auto& comp : wto) {
    loop_impl::visit_depth_first<std::string>(
        comp, [&s](const std::string& v) { s << v; });
    s << std::endl;
  }

  std::ostringstream check;
  check << 1 << std::endl
        << 2 << std::endl
        << 34567 << std::endl
        << 8 << std::endl;
  EXPECT_EQ(s.str(), check.str());
}

TEST_F(LoopInfoTest, construct_level_order_traversal) {
  // 1 2 (3 4 (5 6) 7) 8 (9)
  SimpleGraph g;
  g.add_edge("1", "2");
  g.add_edge("2", "3");
  g.add_edge("3", "4");
  g.add_edge("4", "5");
  g.add_edge("5", "6");
  g.add_edge("6", "7");
  g.add_edge("7", "8");
  g.add_edge("2", "8");
  g.add_edge("4", "7");
  g.add_edge("6", "5");
  g.add_edge("7", "3");
  g.add_edge("8", "9");
  g.add_edge("9", "9");

  sparta::WeakTopologicalOrdering<std::string> wto(
      "1", [&g](const std::string& n) { return g.successors(n); });

  std::vector<std::reference_wrapper<const sparta::WtoComponent<std::string>>>
      level_order;

  loop_impl::construct_level_order_traversal(level_order, wto);

  EXPECT_EQ(level_order.at(0).get().head_node(), "3");
  EXPECT_EQ(level_order.at(1).get().head_node(), "9");
  EXPECT_EQ(level_order.at(2).get().head_node(), "5");
}
