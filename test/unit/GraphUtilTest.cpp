/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "GraphUtil.h"

#include <gtest/gtest.h>
#include <unordered_map>

#include "SimpleGraph.h"

using namespace graph;

/*
 *  +-> 1 -+
 *  |      v
 *  0      3
 *  |      ^
 *  +-> 2 -+
 */
TEST(GraphUtilTest, postorder_diamond) {
  GraphInterface::Graph graph;
  graph.add_edge(0, 1);
  graph.add_edge(0, 2);
  graph.add_edge(1, 3);
  graph.add_edge(2, 3);
  const auto& sorted = postorder_sort<GraphInterface>(graph);
  EXPECT_EQ(sorted, std::vector<uint32_t>({3, 2, 1, 0}));
}

/*
 *  +-> 1 -+
 *  |      v
 *  0 <--- 3
 *  |      ^
 *  +-> 2 -+
 */
TEST(GraphUtilTest, postorder_diamond_backedge) {
  GraphInterface::Graph graph;
  graph.add_edge(0, 1);
  graph.add_edge(0, 2);
  graph.add_edge(1, 3);
  graph.add_edge(2, 3);
  graph.add_edge(3, 0);
  const auto& sorted = postorder_sort<GraphInterface>(graph);
  EXPECT_EQ(sorted, std::vector<uint32_t>({3, 2, 1, 0}));
}

/*
 *         +-> 3
 *  +-> 1 -|
 *  |      +-> 4
 *  0
 *  |
 *  +-> 2
 */
TEST(GraphUtilTest, postorder_tree) {
  GraphInterface::Graph graph;
  graph.add_edge(0, 1);
  graph.add_edge(0, 2);
  graph.add_edge(1, 3);
  graph.add_edge(1, 4);
  const auto& sorted = postorder_sort<GraphInterface>(graph);
  EXPECT_EQ(sorted, std::vector<uint32_t>({2, 4, 3, 1, 0}));
}

/*
 *         +-> 3
 *  +-> 1 -|
 *  |      +-> 4
 *  0          |
 *  |          |
 *  +-> 2 <----+
 */
TEST(GraphUtilTest, postorder_tree_crossedge) {
  GraphInterface::Graph graph;
  graph.add_edge(0, 1);
  graph.add_edge(0, 2);
  graph.add_edge(1, 3);
  graph.add_edge(1, 4);
  graph.add_edge(4, 2);
  const auto& sorted = postorder_sort<GraphInterface>(graph);
  EXPECT_EQ(sorted, std::vector<uint32_t>({2, 4, 3, 1, 0}));
}
