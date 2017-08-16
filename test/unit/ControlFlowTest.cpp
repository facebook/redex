/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "ControlFlow.h"

std::ostream& operator<<(std::ostream& os, const Block* b) {
  return os << b->id();
}

std::ostream& operator<<(std::ostream& os, const ControlFlowGraph& cfg) {
  return cfg.write_dot_format(os);
}

TEST(ControlFlow, findExitBlocks) {
  {
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    cfg.set_entry_block(b0);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b0}) << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block(), b0);
  }
  {
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b1}) << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block(), b1);
  }
  {
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b0, EDGE_GOTO);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b0}) << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block(), b0);
  }
  {
    //   +---------+
    //   v         |
    // +---+     +---+     +---+
    // | 0 | --> | 1 | --> | 2 |
    // +---+     +---+     +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b0, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b2}) << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block(), b2);
  }
  {
    //             +---------+
    //             v         |
    // +---+     +---+     +---+
    // | 0 | --> | 1 | --> | 2 |
    // +---+     +---+     +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b1}) << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block(), b1);
  }
  {
    //             +---------+
    //             v         |
    // +---+     +---+     +---+
    // | 0 | --> | 1 | --> | 2 |
    // +---+     +---+     +---+
    //   |
    //   |
    //   v
    // +---+
    // | 3 |
    // +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b0, b3, EDGE_GOTO);
    EXPECT_THAT(find_exit_blocks(cfg), ::testing::UnorderedElementsAre(b1, b3))
        << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block()->id(), 4);
  }
  {
    //             +---------+
    //             v         |
    // +---+     +---+     +---+     +---+
    // | 0 | --> | 1 | --> | 2 | --> | 3 |
    // +---+     +---+     +---+     +---+
    //   ^                             |
    //   +-----------------------------+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b2, b3, EDGE_GOTO);
    cfg.add_edge(b3, b0, EDGE_GOTO);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b0}) << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block(), b0);
  }
  {
    //                 +---------+
    //                 v         |
    //     +---+     +---+     +---+
    //  +- | 0 | --> | 1 | --> | 2 |
    //  |  +---+     +---+     +---+
    //  |
    //  |    +---------+
    //  |    v         |
    //  |  +---+     +---+
    //  +> | 3 | --> | 4 |
    //     +---+     +---+
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    auto b4 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b0, b3, EDGE_GOTO);
    cfg.add_edge(b3, b4, EDGE_GOTO);
    cfg.add_edge(b4, b3, EDGE_GOTO);
    EXPECT_THAT(find_exit_blocks(cfg), ::testing::UnorderedElementsAre(b1, b3))
        << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block()->id(), 5);
  }
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
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    auto b4 = cfg.create_block();
    auto b5 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b0, b3, EDGE_GOTO);
    cfg.add_edge(b3, b4, EDGE_GOTO);
    cfg.add_edge(b4, b3, EDGE_GOTO);
    cfg.add_edge(b4, b5, EDGE_GOTO);
    cfg.add_edge(b2, b5, EDGE_GOTO);
    EXPECT_EQ(find_exit_blocks(cfg), std::vector<Block*>{b5}) << cfg;
    cfg.calculate_exit_block();
    EXPECT_EQ(cfg.exit_block(), b5);
  }
}

TEST(ControlFlow, findImmediateDominator) {
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
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    auto b4 = cfg.create_block();
    auto b5 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b0, b3, EDGE_GOTO);
    cfg.add_edge(b3, b4, EDGE_GOTO);
    cfg.add_edge(b4, b3, EDGE_GOTO);
    cfg.add_edge(b4, b5, EDGE_GOTO);
    cfg.add_edge(b2, b5, EDGE_GOTO);
    auto idom = cfg.immediate_dominators();
    EXPECT_EQ(idom[b0].dom, b0);
    EXPECT_EQ(idom[b1].dom, b0);
    EXPECT_EQ(idom[b3].dom, b0);
    EXPECT_EQ(idom[b2].dom, b1);
    EXPECT_EQ(idom[b4].dom, b3);
    EXPECT_EQ(idom[b5].dom, b0);
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
    ControlFlowGraph cfg;
    auto b0 = cfg.create_block();
    auto b1 = cfg.create_block();
    auto b2 = cfg.create_block();
    auto b3 = cfg.create_block();
    auto b4 = cfg.create_block();
    auto b5 = cfg.create_block();
    cfg.set_entry_block(b0);
    cfg.add_edge(b0, b1, EDGE_GOTO);
    cfg.add_edge(b1, b2, EDGE_GOTO);
    cfg.add_edge(b2, b1, EDGE_GOTO);
    cfg.add_edge(b1, b3, EDGE_GOTO);
    cfg.add_edge(b3, b4, EDGE_GOTO);
    cfg.add_edge(b4, b3, EDGE_GOTO);
    cfg.add_edge(b4, b5, EDGE_GOTO);
    cfg.add_edge(b2, b5, EDGE_GOTO);
    auto idom = cfg.immediate_dominators();
    EXPECT_EQ(idom[b0].dom, b0);
    EXPECT_EQ(idom[b1].dom, b0);
    EXPECT_EQ(idom[b3].dom, b1);
    EXPECT_EQ(idom[b2].dom, b1);
    EXPECT_EQ(idom[b4].dom, b3);
    EXPECT_EQ(idom[b5].dom, b1);
  }
}
