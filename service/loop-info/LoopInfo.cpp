/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoopInfo.h"

using namespace loop_impl;

/**
 * A loop header is the only block inside of the loop with predecessors outside
 * of the loop, and it also dominates all blocks inside of the loop
 */
cfg::Block* Loop::get_header() { return m_blocks.front(); }

/**
 * A loop preheader is the only predecessor of the loop header, and therefore
 * dominates every block inside of the loop.
 */
cfg::Block* Loop::get_preheader() { return m_loop_preheader; }

/**
 * Get the loop immediately outside of the current loop, or nullptr
 */
Loop* Loop::get_parent_loop() { return m_parent_loop; }

void Loop::set_preheader(cfg::Block* ph) { m_loop_preheader = ph; }

bool Loop::contains(Loop* l) const {
  if (l == this) {
    return true;
  }
  if (!l) {
    return false;
  }
  return contains(l->get_parent_loop());
}

bool Loop::contains(cfg::Block* block) const {
  return m_block_set.count(block);
}

/**
 * The loop depth is the number of enclosing loops a loop has (a loop
 * encloses itself). This number will always be >= 1
 */
int Loop::get_loop_depth() const {
  int depth = 1;
  for (auto current = m_parent_loop; current;
       current = current->m_parent_loop) {
    ++depth;
  }
  return depth;
}

/**
 * Populates the unordered set result with blocks that are not in the Loop but
 * that have at least one predecessor inside the loop.
 */
void Loop::get_exit_blocks(std::unordered_set<cfg::Block*>& result) {
  for (auto block : m_blocks) {
    for (auto edge : block->succs()) {
      if (!contains(edge->target())) {
        result.emplace(edge->target());
      }
    }
  }
}

/**
 * Recursively updates the parent_loop fields for this loop and all subloops
 */
void Loop::update_parent_loop_fields() {
  for (auto sub : m_subloops) {
    sub->m_parent_loop = this;
    sub->update_parent_loop_fields();
  }
}

/*
 * Traverses the control flow graph and constructs loop objects for all loops
 * found.
 *
 * In Redex, a Loop is a maximal set of basic blocks that form a
 * strongly connected component with a dedicated header block that dominates
 * all other blocks within the loop.
 *
 * We construct Loops by constructing a weak topological ordering of the
 * control flow graph, and pruning the strongly connected components to find
 * the valid loops.
 */
LoopInfo::LoopInfo(cfg::ControlFlowGraph& cfg) {

  sparta::WeakTopologicalOrdering<cfg::Block*> wto(
      cfg.entry_block(), [&](const cfg::Block* block) {
        std::vector<cfg::Block*> blocks;
        for (auto edge : block->succs()) {
          blocks.emplace_back(edge->target());
        }
        return blocks;
      });

  // construct a level order traversal of a weak topological ordering
  std::vector<ComponentWrapper<cfg::Block*>> level_order;
  construct_level_order_traversal<cfg::Block*>(level_order, wto);

  // Mapping from all blocks that are loop headers and their respective Loop
  // object
  std::unordered_map<const cfg::Block*, Loop*> loop_heads;

  // Iterates through all of the SCCs found by WTO and
  //
  //   1. Checks if the SCC is a valid loop. We do this by checking if the
  //      component head is the only basic block with predecessors outside
  //      of the loop.
  //   2. Constructs a bare bones Loop object for all valid loops.
  //
  for (auto it = level_order.rbegin(); it != level_order.rend(); ++it) {
    std::vector<cfg::Block*> blocks_in_loop;
    std::unordered_set<cfg::Block*> block_set;
    std::unordered_set<Loop*> subloops;
    auto& wto_comp = it->get();

    // construct blocks_in_loop, block_set, and subloops
    visit_depth_first<cfg::Block*>(wto_comp, [&](cfg::Block* block) {
      blocks_in_loop.emplace_back(block);
      block_set.emplace(block);
      if (loop_heads.count(block)) {
        subloops.emplace(loop_heads.at(block));
      }
    });

    // checks if the SCC is a loop
    bool is_loop = true;
    visit_depth_first<cfg::Block*>(wto_comp, [&](cfg::Block* block) {
      if (!block_set.count(block)) {
        is_loop = false;
      }
    });

    // SCC is not a loop, just break early
    if (!is_loop) {
      continue;
    }

    always_assert(blocks_in_loop.size() != 0);
    auto loop_header = blocks_in_loop.front();
    auto& loop_header_preds = loop_header->preds();
    auto loop_preheader = cfg.create_block();

    for (auto edge : loop_header_preds) {
      if (!block_set.count(edge->src())) {
        cfg.set_edge_target(edge, loop_preheader);
      }
    }

    // connect the preheader with the header
    auto edge =
        new cfg::Edge(loop_preheader, loop_header, cfg::EdgeType::EDGE_GOTO);
    cfg.add_edge(edge);

    auto loop = new Loop(blocks_in_loop, subloops, loop_preheader);

    loop_heads.emplace(loop_header, loop);
    m_loops.emplace_back(loop);
  }

  // we traversed level_order backwards, so reverse loops to make it level
  // order
  std::reverse(m_loops.begin(), m_loops.end());

  // update parent_loop
  for (auto loop : m_loops) {
    // since we are traversing in level order, if this is true then we are done
    if (loop->get_parent_loop() != nullptr) {
      break;
    }
    loop->update_parent_loop_fields();
  }
}

size_t LoopInfo::num_loops() { return m_loops.size(); }
