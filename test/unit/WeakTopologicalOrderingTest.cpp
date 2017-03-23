/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gtest/gtest.h>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

#include "WeakTopologicalOrdering.h"

class SimpleGraph final {
 public:
  SimpleGraph() {}

  void add_edge(std::string source, std::string target) {
    m_edges[source].insert(target);
  }

  std::vector<std::string> successors(std::string node) {
    auto& succs = m_edges[node];
    return std::vector<std::string>(succs.begin(), succs.end());
  }

 private:
  std::unordered_map<std::string, std::set<std::string>> m_edges;
};

/*
 * This graph and the corresponding weak topological ordering are described
 * on page 4 of Bourdoncle's paper:
 *   F. Bourdoncle. Efficient chaotic iteration strategies with widenings.
 *   In Formal Methods in Programming and Their Applications, pp 128-141.
 * The graph is given as follows:
 *
 *                 +-----------------------+
 *                 |           +-----+     |
 *                 |           |     |     |
 *                 V           V     |     |
 *     1 --> 2 --> 3 --> 4 --> 5 --> 6 --> 7 --> 8
 *           |           |                 ^     ^
 *           |           |                 |     |
 *           |           +-----------------+     |
 *           +-----------------------------------+
 *
 * Bourdoncle's algorithm computes the following weak topological ordering:
 *
 *     1 2 (3 4 (5 6) 7) 8
 */
TEST(WeakTopologicalOrderingTest, exampleFromThePaper) {
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

  WeakTopologicalOrdering<std::string> wto(
      "1", [&g](std::string n) { return g.successors(n); });

  std::ostringstream s;
  s << wto;
  EXPECT_EQ("1 2 (3 4 (5 6) 7) 8", s.str());

  auto it = wto.begin();
  EXPECT_EQ("1", it->head_node());
  EXPECT_TRUE(it->is_vertex());
  ++it;
  EXPECT_EQ("2", it->head_node());
  EXPECT_TRUE(it->is_vertex());
  ++it;
  EXPECT_EQ("3", it->head_node());
  EXPECT_TRUE(it->is_scc());
  auto it1 = it->begin();
  EXPECT_EQ("4", it1->head_node());
  EXPECT_TRUE(it1->is_vertex());
  ++it1;
  EXPECT_EQ("5", it1->head_node());
  EXPECT_TRUE(it1->is_scc());
  auto it2 = it1->begin();
  EXPECT_EQ("6", it2->head_node());
  EXPECT_TRUE(it2->is_vertex());
  ++it2;
  EXPECT_TRUE(it2 == it1->end());
  ++it1;
  EXPECT_EQ("7", it1->head_node());
  EXPECT_TRUE(it1->is_vertex());
  ++it1;
  EXPECT_TRUE(it1 == it->end());
  ++it;
  EXPECT_EQ("8", it->head_node());
  EXPECT_TRUE(it->is_vertex());
  ++it;
  EXPECT_TRUE(it == wto.end());
}
