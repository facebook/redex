/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "WeakTopologicalOrdering.h"

#include <gtest/gtest.h>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

using namespace sparta;

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

/*
 * Print the graph in the DOT graph description language. You can use Graphviz
 * or a similar program to render the output.
 */
std::ostream& operator<<(std::ostream& o, const SimpleGraph& g) {
  o << "digraph {\n";
  for (auto& edge : g.m_edges) {
    for (auto& succ : edge.second) {
      o << edge.first << " -> " << succ << "\n";
    }
  }
  o << "}\n";
  return o;
}

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
      "1", [&g](const std::string& n) { return g.successors(n); });

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

/*
 * Check that we correctly handle the edge cases where we have a single-node
 * SCC as the last element of the top-level list of components, or as the last
 * subcomponent in a component
 */
TEST(WeakTopologicalOrderingTest, SingletonSccAtEnd) {
  {
    //             +--+
    //             v  |
    // +---+     +------+
    // | 1 | --> |  2   |
    // +---+     +------+
    SimpleGraph g;
    g.add_edge("1", "2");
    g.add_edge("2", "2");
    WeakTopologicalOrdering<std::string> wto(
        "1", [&g](const std::string& n) { return g.successors(n); });
    std::ostringstream s;
    s << wto;
    EXPECT_EQ("1 (2)", s.str()) << "failed to order graph:\n" << g;
    auto it = wto.begin();
    EXPECT_EQ("1", it->head_node());
    ++it;
    EXPECT_EQ("2", it->head_node());
    EXPECT_TRUE(it->is_scc());
    EXPECT_EQ(it->begin(), it->end());
  }

  {
    //             +--+
    //             v  |
    // +---+     +------+     +---+
    // | 1 | <-- |  2   | --> | 3 |
    // +---+     +------+     +---+
    //   |         ^
    //   +---------+
    SimpleGraph g;
    g.add_edge("1", "2");
    g.add_edge("2", "2");
    g.add_edge("2", "1");
    g.add_edge("2", "3");
    WeakTopologicalOrdering<std::string> wto(
        "1", [&g](const std::string& n) { return g.successors(n); });
    std::ostringstream s;
    s << wto;
    EXPECT_EQ("(1 (2)) 3", s.str()) << "failed to order graph:\n" << g;
    auto it = wto.begin();
    EXPECT_EQ("1", it->head_node());
    auto it1 = it->begin();
    EXPECT_EQ("2", it1->head_node());
    EXPECT_TRUE(it1->is_scc());
    EXPECT_EQ(it1->begin(), it1->end());
  }
}

/*
 * Check that we correctly handle the edge cases where we have a multi-node
 * SCC as the last element of the top-level list of components, or as the last
 * subcomponent in a component
 */
TEST(WeakTopologicalOrderingTest, SccAtEnd) {
  {
    //             +---------+
    //             v         |
    // +---+     +---+     +---+
    // | 1 | --> | 2 | --> | 3 |
    // +---+     +---+     +---+
    SimpleGraph g;
    g.add_edge("1", "2");
    g.add_edge("2", "3");
    g.add_edge("3", "2");

    WeakTopologicalOrdering<std::string> wto(
        "1", [&g](const std::string& n) { return g.successors(n); });
    std::ostringstream s;
    s << wto;
    EXPECT_EQ("1 (2 3)", s.str()) << "failed to order graph:\n" << g;
  }

  {
    //   +-------------------+
    //   |                   v
    // +---+     +---+     +---+     +---+
    // | 2 | <-- | 1 | <-- | 3 | --> | 4 |
    // +---+     +---+     +---+     +---+
    //   ^                   |
    //   +-------------------+
    SimpleGraph g;
    g.add_edge("1", "2");
    g.add_edge("2", "3");
    g.add_edge("3", "2");
    g.add_edge("3", "1");
    g.add_edge("3", "4");

    WeakTopologicalOrdering<std::string> wto(
        "1", [&g](const std::string& n) { return g.successors(n); });
    std::ostringstream s;
    s << wto;
    EXPECT_EQ("(1 (2 3)) 4", s.str()) << "failed to order graph:\n" << g;
  }
}

TEST(WeakTopologicalOrderingTest, SingleNode) {
  {
    // +---+
    // | 1 |
    // +---+
    SimpleGraph g;
    WeakTopologicalOrdering<std::string> wto(
        "1", [&g](const std::string& n) { return g.successors(n); });
    std::ostringstream s;
    s << wto;
    EXPECT_EQ("1", s.str()) << "failed to order graph:\n" << g;
    auto it = wto.begin();
    EXPECT_EQ("1", it->head_node());
    ++it;
    EXPECT_EQ(it, wto.end());
  }
}

TEST(WeakTopologicalOrderingTest, InvalidIteratorDeref) {
  SimpleGraph g;
  g.add_edge("1", "1");
  WeakTopologicalOrdering<std::string> wto(
      "1", [&g](const std::string& n) { return g.successors(n); });
  EXPECT_ANY_THROW(*wto.end());
  EXPECT_ANY_THROW(wto.end()->head_node());
  EXPECT_ANY_THROW(wto.end()++);
}
