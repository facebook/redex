/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Dominators.h"

#include <gtest/gtest.h>

#include "MonotonicFixpointIterator.h"
#include "SimpleGraph.h"

TEST(DominatorsTest, simple) {
  GraphInterface::Graph graph;
  graph.add_edge(0, 1);
  graph.add_edge(0, 2);
  graph.add_edge(1, 3);
  graph.add_edge(1, 4);
  graph.add_edge(4, 2);
  dominators::SimpleFastDominators<GraphInterface> doms(graph);
  EXPECT_EQ(doms.get_idom(1), 0);
  EXPECT_EQ(doms.get_idom(2), 0);
  EXPECT_EQ(doms.get_idom(3), 1);
  EXPECT_EQ(doms.get_idom(4), 1);
}

TEST(DominatorsTest, loop) {
  GraphInterface::Graph graph;
  graph.add_edge(0, 1);
  graph.add_edge(0, 2);
  graph.add_edge(1, 3);
  graph.add_edge(2, 3);
  graph.add_edge(3, 0);
  dominators::SimpleFastDominators<GraphInterface> doms(graph);
  EXPECT_EQ(doms.get_idom(1), 0);
  EXPECT_EQ(doms.get_idom(2), 0);
  EXPECT_EQ(doms.get_idom(3), 0);
}

TEST(GraphUtilTest, doubleLoop) {
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +---+
    //  +- | 0 | --> | 1 | --> | 2 | --> | 5 |
    //  |  +---+     +---+     +---+     +---+
    //  |                                  ^
    //  |    +---------+                   |
    //  |    v         |                   |
    //  |  +---+     +---+                 |
    //  +> | 3 | --> | 4 | ----------------+
    //     +---+     +---+
    GraphInterface::Graph graph;
    graph.add_edge(0, 1);
    graph.add_edge(1, 2);
    graph.add_edge(2, 1);
    graph.add_edge(0, 3);
    graph.add_edge(3, 4);
    graph.add_edge(4, 3);
    graph.add_edge(4, 5);
    graph.add_edge(2, 5);
    dominators::SimpleFastDominators<GraphInterface> doms(graph);
    EXPECT_EQ(doms.get_idom(0), 0);
    EXPECT_EQ(doms.get_idom(1), 0);
    EXPECT_EQ(doms.get_idom(3), 0);
    EXPECT_EQ(doms.get_idom(2), 1);
    EXPECT_EQ(doms.get_idom(4), 3);
    EXPECT_EQ(doms.get_idom(5), 0);
  }
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +---+
    //     | 0 | --> | 1 | --> | 2 | --> | 5 |
    //     +---+     +---+     +---+     +---+
    //                |                    ^
    //  +-------------+                    |
    //  |    +---------+                   |
    //  |    v         |                   |
    //  |  +---+     +---+                 |
    //  +> | 3 | --> | 4 | ----------------+
    //     +---+     +---+
    GraphInterface::Graph graph;
    graph.add_edge(0, 1);
    graph.add_edge(1, 2);
    graph.add_edge(2, 1);
    graph.add_edge(1, 3);
    graph.add_edge(3, 4);
    graph.add_edge(4, 3);
    graph.add_edge(4, 5);
    graph.add_edge(2, 5);
    dominators::SimpleFastDominators<GraphInterface> doms(graph);
    EXPECT_EQ(doms.get_idom(0), 0);
    EXPECT_EQ(doms.get_idom(1), 0);
    EXPECT_EQ(doms.get_idom(3), 1);
    EXPECT_EQ(doms.get_idom(2), 1);
    EXPECT_EQ(doms.get_idom(4), 3);
    EXPECT_EQ(doms.get_idom(5), 1);
  }
}

TEST(GraphUtilTest, postdominator) {
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +----+
    //  +- | 0 | --> | 1 | --> | 2 | --> |100 |
    //  |  +---+     +---+     +---+     +----+
    //  |                                  ^
    //  |    +---------+                   |
    //  |    v         |                   |
    //  |  +---+     +---+                 |
    //  +> | 3 | --> | 4 | ----------------+
    //     +---+     +---+
    GraphInterface::Graph graph;
    graph.add_edge(0, 1);
    graph.add_edge(1, 2);
    graph.add_edge(2, 1);
    graph.add_edge(0, 3);
    graph.add_edge(3, 4);
    graph.add_edge(4, 3);
    graph.add_edge(4, 100);
    graph.add_edge(2, 100);
    dominators::SimpleFastDominators<
        sparta::BackwardsFixpointIterationAdaptor<GraphInterfaceWithExit>>
        post_doms(graph);
    EXPECT_EQ(post_doms.get_idom(0), 100);
    EXPECT_EQ(post_doms.get_idom(1), 2);
    EXPECT_EQ(post_doms.get_idom(3), 4);
    EXPECT_EQ(post_doms.get_idom(2), 100);
    EXPECT_EQ(post_doms.get_idom(4), 100);
    EXPECT_EQ(post_doms.get_idom(100), 100);
  }
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +-----+
    //     | 0 | --> | 1 | --> | 2 | --> | 100 |
    //     +---+     +---+     +---+     +-----+
    //                |                    ^
    //  +-------------+                    |
    //  |    +---------+                   |
    //  |    v         |                   |
    //  |  +---+     +---+                 |
    //  +> | 3 | --> | 4 | ----------------+
    //     +---+     +---+
    GraphInterface::Graph graph;
    graph.add_edge(0, 1);
    graph.add_edge(1, 2);
    graph.add_edge(2, 1);
    graph.add_edge(1, 3);
    graph.add_edge(3, 4);
    graph.add_edge(4, 3);
    graph.add_edge(4, 100);
    graph.add_edge(2, 100);
    dominators::SimpleFastDominators<
        sparta::BackwardsFixpointIterationAdaptor<GraphInterfaceWithExit>>
        post_doms(graph);
    EXPECT_EQ(post_doms.get_idom(0), 1);
    EXPECT_EQ(post_doms.get_idom(1), 100);
    EXPECT_EQ(post_doms.get_idom(3), 4);
    EXPECT_EQ(post_doms.get_idom(2), 100);
    EXPECT_EQ(post_doms.get_idom(4), 100);
    EXPECT_EQ(post_doms.get_idom(100), 100);
  }
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +---+     +---+            +-----+
    //     | 0 | --> | 1 | --> | 2 | --> | 5 | --> | 6 |            | 100 |
    //     +---+     +---+     +---+     +---+     +---+            +-----+
    //                |                    ^       |   |   +---+     ^   ^
    //  +-------------+                    |     +-+   +-->| 8 |     |   |
    //  |    +---------+                   |     |         +---+-----+   |
    //  |    v         |                   |     v                       |
    //  |  +---+     +---+                 |    +---+                    |
    //  +> | 3 | --> | 4 | ----------------+    | 7 |--------------------+
    //     +---+     +---+                      +---+
    GraphInterface::Graph graph;
    graph.add_edge(0, 1);
    graph.add_edge(1, 2);
    graph.add_edge(2, 1);
    graph.add_edge(1, 3);
    graph.add_edge(3, 4);
    graph.add_edge(4, 3);
    graph.add_edge(4, 5);
    graph.add_edge(2, 5);
    graph.add_edge(5, 6);
    graph.add_edge(6, 7);
    graph.add_edge(6, 8);
    graph.add_edge(8, 100);
    graph.add_edge(7, 100);
    dominators::SimpleFastDominators<
        sparta::BackwardsFixpointIterationAdaptor<GraphInterfaceWithExit>>
        post_doms(graph);
    EXPECT_EQ(post_doms.get_idom(0), 1);
    EXPECT_EQ(post_doms.get_idom(1), 5);
    EXPECT_EQ(post_doms.get_idom(3), 4);
    EXPECT_EQ(post_doms.get_idom(2), 5);
    EXPECT_EQ(post_doms.get_idom(4), 5);
    EXPECT_EQ(post_doms.get_idom(5), 6);
    EXPECT_EQ(post_doms.get_idom(6), 100);
    EXPECT_EQ(post_doms.get_idom(7), 100);
    EXPECT_EQ(post_doms.get_idom(8), 100);
    EXPECT_EQ(post_doms.get_idom(100), 100);
  }
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +----+
    //  +- | 0 | --> | 1 | --> | 2 | --> |100 |
    //     +---+     +---+     +---+     +----+
    GraphInterface::Graph graph;
    graph.add_edge(0, 1);
    graph.add_edge(1, 2);
    graph.add_edge(2, 1);
    graph.add_edge(2, 100);
    dominators::SimpleFastDominators<
        sparta::BackwardsFixpointIterationAdaptor<GraphInterfaceWithExit>>
        post_doms(graph);
    EXPECT_EQ(post_doms.get_idom(0), 1);
    EXPECT_EQ(post_doms.get_idom(1), 2);
    EXPECT_EQ(post_doms.get_idom(2), 100);
    EXPECT_EQ(post_doms.get_idom(100), 100);
  }
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+     +----+
    //  +- | 0 | --> | 1 | --> | 2 | --> |100 |
    //     +---+     +---+     +---+     +----+
    //                 |                   ^
    //                 |                   |
    //                 +-------------------+
    GraphInterface::Graph graph;
    graph.add_edge(0, 1);
    graph.add_edge(1, 2);
    graph.add_edge(2, 1);
    graph.add_edge(2, 100);
    graph.add_edge(1, 100);
    dominators::SimpleFastDominators<
        sparta::BackwardsFixpointIterationAdaptor<GraphInterfaceWithExit>>
        post_doms(graph);
    EXPECT_EQ(post_doms.get_idom(0), 1);
    EXPECT_EQ(post_doms.get_idom(1), 100);
    EXPECT_EQ(post_doms.get_idom(2), 100);
    EXPECT_EQ(post_doms.get_idom(100), 100);
  }
}
