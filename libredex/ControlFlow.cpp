/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ControlFlow.h"

#include <boost/numeric/conversion/cast.hpp>
#include <stack>

#include "DexUtil.h"
#include "Transform.h"

namespace {

bool end_of_block(const IRCode* code,
                  FatMethod::iterator it,
                  bool in_try,
                  bool end_block_before_throw) {
  auto next = std::next(it);
  if (next == code->cend()) {
    return true;
  }
  if (next->type == MFLOW_TARGET || next->type == MFLOW_TRY ||
      next->type == MFLOW_CATCH) {
    return true;
  }
  if (end_block_before_throw) {
    if (in_try && it->type == MFLOW_FALLTHROUGH &&
        it->throwing_mie != nullptr) {
      return true;
    }
  } else {
    if (in_try && it->type == MFLOW_OPCODE &&
        opcode::may_throw(it->insn->opcode())) {
      return true;
    }
  }
  if (it->type != MFLOW_OPCODE) {
    return false;
  }
  if (is_branch(it->insn->opcode()) || is_return(it->insn->opcode()) ||
      it->insn->opcode() == OPCODE_THROW) {
    return true;
  }
  return false;
}

void split_may_throw(IRCode* code, FatMethod::iterator it) {
  auto& mie = *it;
  if (mie.type == MFLOW_OPCODE && opcode::may_throw(mie.insn->opcode())) {
    code->insert_before(it, *MethodItemEntry::make_throwing_fallthrough(&mie));
  }
}

} // namespace

ControlFlowGraph::ControlFlowGraph(IRCode* code,
                                   bool end_block_before_throw) {
  // Find the block boundaries
  std::unordered_map<MethodItemEntry*, std::vector<Block*>> branch_to_targets;
  std::vector<std::pair<TryEntry*, Block*>> try_ends;
  std::unordered_map<CatchEntry*, Block*> try_catches;
  std::vector<Block*> exit_blocks;
  bool in_try = false;

  auto* block = create_block();
  always_assert_log(code->count_opcodes() > 0, "FatMethod contains no instructions");
  block->m_begin = code->begin();
  set_entry_block(block);
  // The first block can be a branch target.
  auto begin = code->begin();
  if (begin->type == MFLOW_TARGET) {
    branch_to_targets[begin->target->src].push_back(block);
  }
  for (auto it = code->begin(); it != code->end(); ++it) {
    split_may_throw(code, it);
  }
  for (auto it = code->begin(); it != code->end(); ++it) {
    if (it->type == MFLOW_TRY) {
      if (it->tentry->type == TRY_START) {
        in_try = true;
      } else if (it->tentry->type == TRY_END) {
        in_try = false;
      }
    }
    if (!end_of_block(code, it, in_try, end_block_before_throw)) {
      continue;
    }
    // End the current block.
    auto next = std::next(it);
    block->m_end = next;
    if (next == code->end()) {
      break;
    }
    // Start a new block at the next MethodItem.
    block = create_block();
    block->m_begin = next;
    // Record branch targets to add edges in the next pass.
    if (next->type == MFLOW_TARGET) {
      // If there is a consecutive list of MFLOW_TARGETs, put them all in the
      // same basic block. Being parsimonious in the number of BBs we generate
      // is a significant performance win for our analyses.
      do {
        branch_to_targets[next->target->src].push_back(block);
      } while (++next != code->end() && next->type == MFLOW_TARGET);
      // for the next iteration of the for loop, we want `it` to point to the
      // last of the series of MFLOW_TARGET mies. Since `next` is currently
      // pointing to the mie *after* that element, and since `it` will be
      // incremented on every iteration, we need to decrement by 2 here.
      it = std::prev(next, 2);
    // Record try/catch blocks to add edges in the next pass.
    } else if (next->type == MFLOW_TRY && next->tentry->type == TRY_END) {
      try_ends.emplace_back(next->tentry, block);
    } else if (next->type == MFLOW_CATCH) {
      // If there is a consecutive list of MFLOW_CATCHes, put them all in the
      // same basic block.
      do {
        try_catches[next->centry] = block;
      } while (++next != code->end() && next->type == MFLOW_CATCH);
      it = std::prev(next, 2);
    }
  }
  // Link the blocks together with edges
  for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
    // Set outgoing edge if last MIE falls through
    auto lastmei = (*it)->rbegin();
    bool fallthrough = true;
    if (lastmei->type == MFLOW_OPCODE) {
      auto lastop = lastmei->insn->opcode();
      if (is_branch(lastop)) {
        fallthrough = !is_goto(lastop);
        auto const& targets = branch_to_targets[&*lastmei];
        for (auto target : targets) {
          add_edge(
              *it, target, is_goto(lastop) ? EDGE_GOTO : EDGE_BRANCH);
        }
      } else if (is_return(lastop) || lastop == OPCODE_THROW) {
        fallthrough = false;
      }
    }
    if (fallthrough && std::next(it) != m_blocks.end()) {
      Block* next = *std::next(it);
      add_edge(*it, next, EDGE_GOTO);
    }
  }
  /*
   * Now add the catch edges.  Every block inside a try-start/try-end region
   * gets an edge to every catch block.  This simplifies dataflow analysis
   * since you can always get the exception state by looking at successors,
   * without any additional analysis.
   *
   * NB: This algorithm assumes that a try-start/try-end region will consist of
   * sequentially-numbered blocks, which is guaranteed because catch regions
   * are contiguous in the bytecode, and we generate blocks in bytecode order.
   */
  for (auto tep : try_ends) {
    auto try_end = tep.first;
    auto tryendblock = tep.second;
    size_t bid = tryendblock->id();
    always_assert(bid > 0);
    --bid;
    while (true) {
      block = m_blocks.at(bid);
      if (ends_with_may_throw(block, end_block_before_throw)) {
        for (auto mei = try_end->catch_start;
             mei != nullptr;
             mei = mei->centry->next) {
          auto catchblock = try_catches.at(mei->centry);
          add_edge(block, catchblock, EDGE_THROW);
        }
      }
      auto block_begin = block->begin();
      if (block_begin->type == MFLOW_TRY) {
        auto tentry = block_begin->tentry;
        if (tentry->type == TRY_START) {
          always_assert(tentry->catch_start == try_end->catch_start);
          break;
        }
      }
      always_assert_log(bid > 0, "No beginning of try region found");
      --bid;
    }
  }
  // Remove edges between unreachable blocks and their succ blocks.
  std::unordered_set<Block*> visited;
  transform::visit(m_blocks.at(0), visited);
  for (size_t i = 1; i < m_blocks.size(); ++i) {
    auto& b = m_blocks.at(i);
    if (visited.find(b) != visited.end()) {
      continue;
    }
    transform::remove_succ_edges(b, this);
  }
  TRACE(CFG, 5, "%s", SHOW(*this));
}

ControlFlowGraph::~ControlFlowGraph() {
  for (auto block : m_blocks) {
    delete block;
  }
}

Block* ControlFlowGraph::create_block() {
  m_blocks.emplace_back(new Block(m_blocks.size()));
  return m_blocks.back();
}

void ControlFlowGraph::calculate_exit_block() {
  if (m_exit_block != nullptr) {
    return;
  }
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

void ControlFlowGraph::add_edge(Block* p, Block* s, EdgeType type) {
  if (std::find(p->succs().begin(), p->succs().end(), s) == p->succs().end()) {
    p->m_succs.push_back(s);
    s->m_preds.push_back(p);
  }
  mutable_edge(p, s).set(type);
}

void ControlFlowGraph::remove_edge(Block* p, Block* s, EdgeType type) {
  mutable_edge(p, s).reset(type);
  if (edge(p, s).none()) {
    remove_all_edges(p, s);
  }
}

void ControlFlowGraph::remove_all_edges(Block* p, Block* s) {
  mutable_edge(p, s).reset();
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

bool ends_with_may_throw(Block* p, bool end_block_before_throw) {
  if (!end_block_before_throw) {
    for (auto last = p->rbegin(); last != p->rend(); ++last) {
      if (last->type != MFLOW_OPCODE) {
        continue;
      }
      return last->insn->opcode() == OPCODE_THROW ||
             opcode::may_throw(last->insn->opcode());
    }
  }
  for (auto last = p->rbegin(); last != p->rend(); ++last) {
    switch (last->type) {
    case MFLOW_FALLTHROUGH:
      if (last->throwing_mie) {
        return true;
      }
      break;
    case MFLOW_OPCODE:
      if (last->insn->opcode() == OPCODE_THROW) {
        return true;
      } else {
        return false;
      }
    case MFLOW_TRY:
    case MFLOW_CATCH:
    case MFLOW_TARGET:
    case MFLOW_POSITION:
    case MFLOW_DEBUG:
      break;
    }
  }
  return false;
}

std::vector<Block*> postorder_sort(const std::vector<Block*>& cfg) {
  std::vector<Block*> postorder;
  std::vector<Block*> stack;
  std::unordered_set<Block*> visited;
  for (size_t i = 1; i < cfg.size(); i++) {
    if (cfg[i]->preds().size() == 0) {
      stack.push_back(cfg[i]);
    }
  }
  stack.push_back(cfg[0]);
  while (!stack.empty()) {
    auto const& curr = stack.back();
    visited.insert(curr);
    bool all_succs_visited = [&] {
      for (auto const& s : curr->succs()) {
        if (!visited.count(s)) {
          stack.push_back(s);
          return false;
        }
      }
      return true;
    }();
    if (all_succs_visited) {
      assert(curr == stack.back());
      postorder.push_back(curr);
      stack.pop_back();
    }
  }
  return postorder;
}

Block* ControlFlowGraph::find_block_that_ends_here(
    const FatMethod::iterator& loc) const {
  for (Block* b : m_blocks) {
    if (b->m_end == loc) {
      return b;
    }
  }
  return nullptr;
}

Block* ControlFlowGraph::idom_intersect(
    const std::unordered_map<Block*, DominatorInfo>& postorder_dominator,
    Block* block1,
    Block* block2) const {
  auto finger1 = block1;
  auto finger2 = block2;
  while (finger1 != finger2) {
    while (postorder_dominator.at(finger1).postorder <
           postorder_dominator.at(finger2).postorder) {
      finger1 = postorder_dominator.at(finger1).dom;
    }
    while (postorder_dominator.at(finger2).postorder <
           postorder_dominator.at(finger1).postorder) {
      finger2 = postorder_dominator.at(finger2).dom;
    }
  }
  return finger1;
}

// Finding immediate dominator for each blocks in ControlFlowGraph.
// Theory from:
//    K. D. Cooper et.al. A Simple, Fast Dominance Algorithm.
std::unordered_map<Block*, DominatorInfo>
ControlFlowGraph::immediate_dominators() const {
  // Get postorder of blocks and create map of block to postorder number.
  std::unordered_map<Block*, DominatorInfo> postorder_dominator;
  auto postorder_blocks = postorder_sort(blocks());
  for (size_t i = 0; i < postorder_blocks.size(); ++i) {
    postorder_dominator[postorder_blocks[i]].postorder = i;
  }

  // Initialize immediate dominators. Having value as nullptr means it has
  // not been processed yet.
  for (Block* block : blocks()) {
    if (block->preds().size() == 0) {
      // Entry block's immediate dominator is itself.
      postorder_dominator[block].dom = block;
    } else {
      postorder_dominator[block].dom = nullptr;
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    // Traverse block in reverse postorder.
    for (auto rit = postorder_blocks.rbegin(); rit != postorder_blocks.rend();
         ++rit) {
      Block* ordered_block = *rit;
      if (ordered_block->preds().size() == 0) {
        continue;
      }
      Block* new_idom = nullptr;
      // Pick one random processed block as starting point.
      for (auto& pred : ordered_block->preds()) {
        if (postorder_dominator[pred].dom != nullptr) {
          new_idom = pred;
          break;
        }
      }
      always_assert(new_idom != nullptr);
      for (auto& pred : ordered_block->preds()) {
        if (pred != new_idom && postorder_dominator[pred].dom != nullptr) {
          new_idom = idom_intersect(postorder_dominator, new_idom, pred);
        }
      }
      if (postorder_dominator[ordered_block].dom != new_idom) {
        postorder_dominator[ordered_block].dom = new_idom;
        changed = true;
      }
    }
  }
  return postorder_dominator;
}
