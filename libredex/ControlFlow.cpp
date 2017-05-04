/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ControlFlow.h"

ControlFlowGraph::~ControlFlowGraph() {
  for (auto block : m_blocks) {
    delete block;
  }
}

Block* ControlFlowGraph::create_block() {
  m_blocks.emplace_back(new Block(m_blocks.size()));
  return m_blocks.back();
}

void ControlFlowGraph::add_edge(Block* p, Block* s, EdgeType type) {
  if (std::find(p->succs().begin(), p->succs().end(), s) == p->succs().end()) {
    p->m_succs.push_back(s);
    s->m_preds.push_back(p);
  }
  edge(p, s).set(type);
}

void ControlFlowGraph::remove_edge(Block* p, Block* s, EdgeType type) {
  edge(p, s).reset(type);
  if (edge(p, s).none()) {
    remove_all_edges(p, s);
  }
}

void ControlFlowGraph::remove_all_edges(Block* p, Block* s) {
  edge(p, s).reset();
  p->m_succs.erase(std::remove_if(p->m_succs.begin(),
                                  p->m_succs.end(),
                                  [&](Block* b) { return b == s; }),
                   p->succs().end());
  s->m_preds.erase(std::remove_if(s->m_preds.begin(),
                                  s->m_preds.end(),
                                  [&](Block* b) { return b == p; }),
                   s->preds().end());
}

std::ostream& ControlFlowGraph::write_dot_format(std::ostream& o) const {
  o << "digraph {\n";
  for (auto* block : blocks()) {
    for (auto* succ : block->succs()) {
      o << block->id() << " -> " << succ->id() << "\n";
    }
  }
  o << "}\n";
  return o;
}

/*
 * Find all exit blocks. Note that it's not as simple as looking for BBs with
 * return or throw opcodes; infinite loops are a valid way of terminating dex
 * bytecode too. As such, we need to find all SCCs and vertices that lack
 * successors. For SCCs that lack successors, any one of its vertices can be
 * treated as an exit block; the implementation below picks the head of the
 * SCC.
 */
std::vector<Block*> find_exit_blocks(const ControlFlowGraph& cfg) {
  std::vector<Block*> exit_blocks;
  uint32_t next_dfn {0};
  std::stack<const Block*> stack;
  // Depth-first number. Special values:
  //   0 - unvisited
  //   UINT32_MAX - visited and determined to be in a separate SCC
  std::unordered_map<const Block*, uint32_t> dfns;
  constexpr uint32_t VISITED = std::numeric_limits<uint32_t>::max();
  // This is basically Tarjan's algorithm for finding SCCs. I pass around an
  // extra has_exit value to determine if a given SCC has any successors.
  using t = std::pair<uint32_t, bool>;
  std::function<t(const Block*)> visit = [&](const Block* b) {
    stack.push(b);
    uint32_t head = dfns[b] = ++next_dfn;
    // whether any vertex in the current SCC has a successor edge that points
    // outside itself
    bool has_exit {false};
    for (auto* succ : b->succs()) {
      uint32_t succ_dfn = dfns[succ];
      uint32_t min;
      if (succ_dfn == 0) {
        bool succ_has_exit;
        std::tie(min, succ_has_exit) = visit(succ);
        has_exit |= succ_has_exit;
      } else {
        has_exit |= succ_dfn == VISITED;
        min = succ_dfn;
      }
      head = std::min(min, head);
    }
    if (head == dfns[b]) {
      const Block* top {nullptr};
      if (!has_exit) {
        exit_blocks.push_back(const_cast<Block*>(b));
        has_exit = true;
      }
      do {
        top = stack.top();
        stack.pop();
        dfns[top] = VISITED;
      } while (top != b);
    }
    return t(head, has_exit);
  };
  visit(cfg.entry_block());
  return exit_blocks;
}

void ControlFlowGraph::calculate_exit_block() {
  auto exit_blocks = find_exit_blocks(*this);
  if (exit_blocks.size() == 1) {
    m_exit_block = exit_blocks.at(0);
  } else {
    auto ghost_exit_block = create_block();
    set_exit_block(ghost_exit_block);
    for (auto* b : exit_blocks) {
      add_edge(b, ghost_exit_block, EDGE_GOTO);
    }
  }
}
