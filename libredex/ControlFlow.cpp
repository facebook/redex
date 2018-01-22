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

using namespace cfg;

namespace {

bool end_of_block(const FatMethod* fm, FatMethod::iterator it, bool in_try) {
  auto next = std::next(it);
  if (next == fm->end()) {
    return true;
  }
  if (next->type == MFLOW_TARGET || next->type == MFLOW_TRY ||
      next->type == MFLOW_CATCH) {
    return true;
  }
  if (in_try && it->type == MFLOW_OPCODE &&
      opcode::may_throw(it->insn->opcode())) {
    return true;
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

bool ends_with_may_throw(Block* p) {
  for (auto last = p->rbegin(); last != p->rend(); ++last) {
    if (last->type != MFLOW_OPCODE) {
      continue;
    }
    return last->insn->opcode() == OPCODE_THROW ||
           opcode::may_throw(last->insn->opcode());
  }
  return false;
}

} // namespace

FatMethod::iterator Block::begin() {
  if (m_parent->editable()) {
    return m_entries.begin();
  } else {
    return m_begin;
  }
}

FatMethod::iterator Block::end() {
  if (m_parent->editable()) {
    return m_entries.end();
  } else {
    return m_end;
  }
}

ControlFlowGraph::ControlFlowGraph(FatMethod* fm, bool editable)
    : m_editable(editable) {
  always_assert_log(fm->size() > 0, "FatMethod contains no instructions");

  BranchToTargets branch_to_targets;
  TryEnds try_ends;
  TryCatches try_catches;
  std::vector<Block*> exit_blocks;
  Boundaries boundaries; // block boundaries (for editable == true)

  find_block_boundaries(
      fm, branch_to_targets, try_ends, try_catches, boundaries);

  if (m_editable) {
    fill_blocks(fm, boundaries);
  }

  connect_blocks(branch_to_targets);
  add_catch_edges(try_ends, try_catches);
  if (m_editable) {
    remove_try_markers();
  }

  remove_unreachable_succ_edges();

  if (m_editable) {
    add_fallthrough_gotos();
    sanity_check();
  }

  TRACE(CFG, 5, "editable %d, %s", m_editable, SHOW(*this));
}

void ControlFlowGraph::find_block_boundaries(FatMethod* fm,
                                             BranchToTargets& branch_to_targets,
                                             TryEnds& try_ends,
                                             TryCatches& try_catches,
                                             Boundaries& boundaries) {
  // Find the block boundaries
  auto* block = create_block();
  if (m_editable) {
    boundaries[block].first = fm->begin();
  } else {
    block->m_begin = fm->begin();
  }

  set_entry_block(block);
  // The first block can be a branch target.
  auto begin = fm->begin();
  if (begin->type == MFLOW_TARGET) {
    branch_to_targets[begin->target->src].push_back(block);
  }
  MethodItemEntry* active_try = nullptr;
  for (auto it = fm->begin(); it != fm->end(); ++it) {
    if (it->type == MFLOW_TRY) {
      // Assumption: MFLOW_TRYs are only at the beginning of blocks
      always_assert(!m_editable || it == boundaries[block].first);
      always_assert(m_editable || it == block->m_begin);
      if (it->tentry->type == TRY_START) {
        active_try = it->tentry->catch_start;
      } else if (it->tentry->type == TRY_END) {
        active_try = nullptr;
      }
      block->m_catch_start = active_try;
    }
    if (!end_of_block(fm, it, active_try != nullptr)) {
      continue;
    }

    // End the current block.
    auto next = std::next(it);
    if (m_editable) {
      boundaries[block].second = next;
    } else {
      block->m_end = next;
    }

    if (next == fm->end()) {
      break;
    }

    // Start a new block at the next MethodItem.
    block = create_block();
    if (m_editable) {
      boundaries[block].first = next;
    } else {
      block->m_begin = next;
    }
    block->m_catch_start = active_try;
    // Record branch targets to add edges in the next pass.
    if (next->type == MFLOW_TARGET) {
      // If there is a consecutive list of MFLOW_TARGETs, put them all in the
      // same basic block. Being parsimonious in the number of BBs we generate
      // is a significant performance win for our analyses.
      do {
        branch_to_targets[next->target->src].push_back(block);
      } while (++next != fm->end() && next->type == MFLOW_TARGET);
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
      } while (++next != fm->end() && next->type == MFLOW_CATCH);
      it = std::prev(next, 2);
    }
  }
  TRACE(CFG, 5, "  build: boundaries found\n");
}

void ControlFlowGraph::connect_blocks(BranchToTargets& branch_to_targets) {
  // Link the blocks together with edges
  for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
    // Set outgoing edge if last MIE falls through
    Block* b = it->second;
    auto lastmei = b->rbegin();
    bool fallthrough = true;
    if (lastmei->type == MFLOW_OPCODE) {
      auto lastop = lastmei->insn->opcode();
      if (is_branch(lastop)) {
        fallthrough = lastop != OPCODE_GOTO;
        auto const& targets = branch_to_targets[&*lastmei];
        for (auto target : targets) {
          add_edge(b, target, lastop == OPCODE_GOTO ? EDGE_GOTO : EDGE_BRANCH);
        }
      } else if (is_return(lastop) || lastop == OPCODE_THROW) {
        fallthrough = false;
      }
    }

    auto next = std::next(it);
    Block* next_b = next->second;
    if (fallthrough && next != m_blocks.end()) {
      TRACE(CFG,
            5,
            "setting default successor %d -> %d\n",
            b->id(),
            next_b->id());
      b->m_default_successor = next_b;
      add_edge(b, next_b, EDGE_GOTO);
    }
  }
  TRACE(CFG, 5, "  build: edges added\n");
}

void ControlFlowGraph::add_catch_edges(TryEnds& try_ends,
                                       TryCatches& try_catches) {
  /*
   * Every block inside a try-start/try-end region
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
      Block* block = m_blocks.at(bid);
      if (ends_with_may_throw(block)) {
        for (auto mei = try_end->catch_start; mei != nullptr;
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
  TRACE(CFG, 5, "  build: catch edges added\n");
}

void ControlFlowGraph::remove_unreachable_succ_edges() {
  // Remove edges between unreachable blocks and their succ blocks.
  std::unordered_set<Block*> visited;
  transform::visit(m_entry_block, visited);
  for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
    Block* b = it->second;
    if (visited.find(b) != visited.end()) {
      continue;
    }

    TRACE(CFG, 5, "  build: removing succ edges from block %d\n", b->id());
    remove_succ_edges(b);
  }
  TRACE(CFG, 5, "  build: unreachables removed\n");
}

// Move the `MethodItemEntry`s from `fm` into the blocks, based on the
// information in `boundaries`.
//
// The CFG takes ownership of the `MethodItemEntry`s and `fm` is left empty.
void ControlFlowGraph::fill_blocks(FatMethod* fm, Boundaries& boundaries) {
  always_assert(m_editable);
  // fill the blocks between their boundaries
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    b->m_entries.splice(b->m_entries.end(),
                        *fm,
                        boundaries.at(b).first,
                        boundaries.at(b).second);
  }
  TRACE(CFG, 5, "  build: splicing finished\n");
}

void ControlFlowGraph::add_fallthrough_gotos() {
  always_assert(m_editable);
  // Assumption: m_blocks is still in original execution order.
  for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
    Block* b = it->second;
    auto next = std::next(it);
    Block* next_b = next->second;
    if (next != m_blocks.end() && !b->succs().empty() &&
        b->rbegin()->branchingness() == opcode::BRANCH_NONE) {
      MethodItemEntry* fallthrough_goto =
          new MethodItemEntry(new IRInstruction(OPCODE_GOTO));
      MethodItemEntry* fallthrough_target =
          new MethodItemEntry(new BranchTarget(fallthrough_goto));
      b->m_entries.push_back(*fallthrough_goto);
      next_b->m_entries.push_front(*fallthrough_target);
    }
  }
}

void ControlFlowGraph::sanity_check() {
  always_assert(m_editable);
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    if (!b->m_succs.empty()) {
      always_assert_log(b->m_entries.rbegin()->branchingness() !=
                            opcode::BRANCH_NONE,
                        "Block ends with BRANCH_NONE:\n%s",
                        SHOW(&b->m_entries));
    }
  }
}

// remove any MFLOW_TARGETs that don't have a corresponding branch
void ControlFlowGraph::clean_dangling_targets() {
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;

    // find all branch instructions in all predecessors
    std::unordered_set<MethodItemEntry*> branches;
    for (auto& pred : b->m_preds) {
      for (auto& mie : pred->src()->m_entries) {
        if (mie.branchingness() != opcode::BRANCH_NONE) {
          branches.insert(&mie);
        }
      }
    }

    // Now look for labels that aren't in `branches`
    for (auto it = b->m_entries.begin(); it != b->m_entries.end();) {
      if (it->type == MFLOW_TARGET &&
          branches.find(it->target->src) == branches.end()) {
        // Found a dangling label, delete it
        it = b->m_entries.erase_and_dispose(it, FatMethodDisposer());
      } else {
        ++it;
      }
    }
  }
}

std::vector<Block*> ControlFlowGraph::order() {
  // TODO output in a better order
  // The order should maximize PC locality, hot blocks should be fallthroughs
  // and cold blocks (like exception handlers) should be jumps.
  //
  // We want something similar to reverse post order but RPO isn't well defined
  // on cyclic graphs.
  //   (A) First, it finds Strongly Connected Components (similar to WTO)
  //   (B) It adds a node to the order upon the first traversal, not after
  //       reaching it from ALL predecessors (as a topographical sort requires).
  //       For example, we want catch blocks at the end, after the return block
  //       that they may jump to.
  //   (C) It recurses into a SCC before considering successors of the SCC
  //   (D) It places default successors immediately after
  return blocks(); // this is just id order (same as input order)
}

FatMethod::iterator Block::get_goto() {
  for (auto it = m_entries.begin(); it != m_entries.end(); it++) {
    if (it->type == MFLOW_OPCODE && it->insn->opcode() == OPCODE_GOTO) {
      return it;
    }
  }
  return m_entries.end();
}

std::vector<FatMethod::iterator> Block::get_targets() {
  std::vector<FatMethod::iterator> result;
  for (auto it = m_entries.begin(); it != m_entries.end(); it++) {
    if (it->type == MFLOW_TARGET) {
      result.emplace_back(it);
    }
  }
  return result;
}

void ControlFlowGraph::remove_fallthrough_gotos(
    const std::vector<Block*> ordering) {
  // remove unnecesary GOTOs
  Block* prev = nullptr;
  for (auto it = ordering.begin(); it != ordering.end(); prev = *(it++)) {
    Block* b = *it;
    if (prev == nullptr) {
      continue;
    }

    FatMethod::iterator prev_goto = prev->get_goto();
    if (prev_goto != prev->m_entries.end()) {
      std::vector<FatMethod::iterator> targets = b->get_targets();
      for (const FatMethod::iterator& target : targets) {
        if (target->target->src == &*prev_goto) {
          // found a fallthrough goto. Remove the goto and the target
          prev->m_entries.erase_and_dispose(prev_goto, FatMethodDisposer());
          b->m_entries.erase_and_dispose(target, FatMethodDisposer());
        }
      }
    }
  }
}

// remove all TRY START and ENDs because we may reorder the blocks
void ControlFlowGraph::remove_try_markers() {
  always_assert(m_editable);
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    b->m_entries.remove_and_dispose_if(
        [b](const MethodItemEntry& mie) {
          if (mie.type == MFLOW_TRY) {
            // make sure we're not losing any information
            if (mie.tentry->type == TRY_START) {
              always_assert(b->m_catch_start == mie.tentry->catch_start);
            } else if (mie.tentry->type == TRY_END) {
              always_assert(b->m_catch_start == nullptr);
            } else {
              always_assert_log(false, "Bogus MethodItemEntry MFLOW_TRY");
            }
            // delete tries
            return true;
          }
          // leave everything else
          return false;
        },
        FatMethodDisposer());
  }
}

FatMethod* ControlFlowGraph::linearize() {
  FatMethod* result = new FatMethod;

  TRACE(CFG, 5, "before linearize:\n");
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    TRACE(CFG, 5, "%s", SHOW(&(b->m_entries)));
  }

  const std::vector<Block*>& ordering = order();
  remove_fallthrough_gotos(ordering);

  // add back the TRY START and ENDS
  Block* prev = nullptr;
  for (auto it = ordering.begin(); it != ordering.end(); prev = *(it++)) {
    Block* b = *it;
    if (prev == nullptr || b->m_catch_start != prev->m_catch_start) {
      // current try changes upon entering this block.
      // end the previous try
      if (prev != nullptr && prev->m_catch_start != nullptr) {
        prev->m_entries.push_back(
            *new MethodItemEntry(TRY_END, prev->m_catch_start));
      }
      if (b->m_catch_start != nullptr) {
        b->m_entries.push_front(
            *new MethodItemEntry(TRY_START, b->m_catch_start));
      }
    }

    // and end the last block too
    if (std::next(it) == ordering.end() && b->m_catch_start != nullptr) {
      b->m_entries.push_back(*new MethodItemEntry(TRY_END, b->m_catch_start));
    }
  }

  for (Block* b : ordering) {
    result->splice(result->end(), b->m_entries);
  }

  return result;
}

std::vector<Block*> ControlFlowGraph::blocks() const {
  std::vector<Block*> result;
  result.reserve(m_blocks.size());
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    result.emplace_back(b);
  }
  return result;
}

ControlFlowGraph::~ControlFlowGraph() {
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    delete b;
  }
}

Block* ControlFlowGraph::create_block() {
  size_t id = m_blocks.size();
  Block* b = new Block(this, id);
  m_blocks.emplace(id, b);
  return b;
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
  auto edge = std::make_shared<Edge>(p, s, type);
  p->m_succs.emplace_back(edge);
  s->m_preds.emplace_back(edge);
}

void ControlFlowGraph::remove_all_edges(Block* p, Block* s) {
  p->m_succs.erase(std::remove_if(p->m_succs.begin(),
                                  p->m_succs.end(),
                                  [&](const std::shared_ptr<Edge>& e) {
                                    return e->target() == s;
                                  }),
                   p->succs().end());
  s->m_preds.erase(std::remove_if(s->m_preds.begin(),
                                  s->m_preds.end(),
                                  [&](const std::shared_ptr<Edge>& e) {
                                    return e->src() == p;
                                  }),
                   s->preds().end());
}

std::ostream& ControlFlowGraph::write_dot_format(std::ostream& o) const {
  o << "digraph {\n";
  for (auto* block : blocks()) {
    for (auto& succ : block->succs()) {
      o << block->id() << " -> " << succ->target()->id() << "\n";
    }
  }
  o << "}\n";
  return o;
}

// We create a small class here (instead of a recursive lambda) so we can
// label visit with NO_SANITIZE_ADDRESS
class ExitBlocks {
 private:
  uint32_t next_dfn{0};
  std::stack<const Block*> stack;
  // Depth-first number. Special values:
  //   0 - unvisited
  //   UINT32_MAX - visited and determined to be in a separate SCC
  std::unordered_map<const Block*, uint32_t> dfns;
  static constexpr uint32_t VISITED = std::numeric_limits<uint32_t>::max();
  // This is basically Tarjan's algorithm for finding SCCs. I pass around an
  // extra has_exit value to determine if a given SCC has any successors.
  using t = std::pair<uint32_t, bool>;

 public:
  std::vector<Block*> exit_blocks;

  NO_SANITIZE_ADDRESS // because of deep recursion. ASAN uses too much memory.
  t visit(const Block* b) {
    stack.push(b);
    uint32_t head = dfns[b] = ++next_dfn;
    // whether any vertex in the current SCC has a successor edge that points
    // outside itself
    bool has_exit{false};
    for (auto& succ : b->succs()) {
      uint32_t succ_dfn = dfns[succ->target()];
      uint32_t min;
      if (succ_dfn == 0) {
        bool succ_has_exit;
        std::tie(min, succ_has_exit) = visit(succ->target());
        has_exit |= succ_has_exit;
      } else {
        has_exit |= succ_dfn == VISITED;
        min = succ_dfn;
      }
      head = std::min(min, head);
    }
    if (head == dfns[b]) {
      const Block* top{nullptr};
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
  }
};

/*
 * Find all exit blocks. Note that it's not as simple as looking for BBs with
 * return or throw opcodes; infinite loops are a valid way of terminating dex
 * bytecode too. As such, we need to find all SCCs and vertices that lack
 * successors. For SCCs that lack successors, any one of its vertices can be
 * treated as an exit block; the implementation below picks the head of the
 * SCC.
 */
std::vector<Block*> find_exit_blocks(const ControlFlowGraph& cfg) {
  ExitBlocks eb{};
  eb.visit(cfg.entry_block());
  return eb.exit_blocks;
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
        if (!visited.count(s->target())) {
          stack.push_back(s->target());
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
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    if (b->end() == loc) {
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
        if (postorder_dominator[pred->src()].dom != nullptr) {
          new_idom = pred->src();
          break;
        }
      }
      always_assert(new_idom != nullptr);
      for (auto& pred : ordered_block->preds()) {
        if (pred->src() != new_idom &&
            postorder_dominator[pred->src()].dom != nullptr) {
          new_idom = idom_intersect(postorder_dominator, new_idom, pred->src());
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

void ControlFlowGraph::remove_succ_edges(Block* b) {
  std::vector<std::pair<Block*, Block*>> remove_edges;
  for (auto& s : b->succs()) {
    remove_edges.emplace_back(b, s->target());
  }
  for (auto& p : remove_edges) {
    this->remove_all_edges(p.first, p.second);
  }
}
