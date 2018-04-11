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
#include <iterator>
#include <stack>
#include <utility>

#include "DexUtil.h"
#include "Transform.h"

namespace {

bool end_of_block(const IRList* ir, IRList::iterator it, bool in_try) {
  auto next = std::next(it);
  if (next == ir->end()) {
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

bool ends_with_may_throw(cfg::Block* p) {
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

namespace cfg {

IRList::iterator Block::begin() {
  if (m_parent->editable()) {
    return m_entries.begin();
  } else {
    return m_begin;
  }
}

IRList::iterator Block::end() {
  if (m_parent->editable()) {
    return m_entries.end();
  } else {
    return m_end;
  }
}

IRList::const_iterator Block::begin() const {
  if (m_parent->editable()) {
    return m_entries.begin();
  } else {
    return m_begin;
  }
}

IRList::const_iterator Block::end() const {
  if (m_parent->editable()) {
    return m_entries.end();
  } else {
    return m_end;
  }
}

void Block::remove_opcode(const IRList::iterator& it) {
  always_assert(m_parent->editable());
  always_assert(it->type == MFLOW_OPCODE);
  always_assert(!is_branch(it->insn->opcode()));
  m_entries.remove_opcode(it);
}

void Block::remove_debug_line_info() {
  for (MethodItemEntry& mie : *this) {
    if (mie.type == MFLOW_POSITION) {
      mie.pos.release();
      mie.type = MFLOW_FALLTHROUGH;
    }
  }
}

bool Block::has_pred(Block* b, EdgeType t) const {
  const auto& edges = preds();
  return std::find_if(edges.begin(), edges.end(), [b, t](const auto& edge) {
           return edge->src() == b &&
                  (t == EDGE_TYPE_SIZE || edge->type() == t);
         }) != edges.end();
}

bool Block::has_succ(Block* b, EdgeType t) const {
  const auto& edges = succs();
  return std::find_if(edges.begin(), edges.end(), [b, t](const auto& edge) {
           return edge->target() == b &&
                  (t == EDGE_TYPE_SIZE || edge->type() == t);
         }) != edges.end();
}

IRList::iterator Block::get_conditional_branch() {
  for (auto it = rbegin(); it != rend(); ++it) {
    if (it->type == MFLOW_OPCODE) {
      auto op = it->insn->opcode();
      if (is_conditional_branch(op) || is_switch(op)) {
        return std::prev(it.base());
      }
    }
  }
  return end();
}

IRList::iterator Block::get_last_insn() {
  for (auto it = rbegin(); it != rend(); ++it) {
    if (it->type == MFLOW_OPCODE) {
      // Reverse iterators have a member base() which returns a corresponding
      // forward iterator. Beware that this isn't an iterator that refers to the
      // same object - it actually refers to the next object in the sequence.
      // This is so that rbegin() corresponds with end() and rend() corresponds
      // with begin(). Copied from https://stackoverflow.com/a/2037917
      return std::prev(it.base());
    }
  }
  return end();
}

IRList::iterator Block::get_first_insn() {
  for (auto it = begin(); it != end(); ++it) {
    if (it->type == MFLOW_OPCODE) {
      return it;
    }
  }
  return end();
}

// We remove the first matching target because multiple switch cases can point
// to the same block. We use this function to move information from the target
// entries to the CFG edges. The two edges are identical, save the case key, so
// it doesn't matter which target is taken. We arbitrarily choose to process the
// targets in forward order.
boost::optional<Edge::CaseKey> Block::remove_first_matching_target(
    MethodItemEntry* branch) {
  for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
    auto& mie = *it;
    if (mie.type == MFLOW_TARGET && mie.target->src == branch) {
      boost::optional<Edge::CaseKey> result;
      if (mie.target->type == BRANCH_MULTI) {
        always_assert_log(is_switch(branch->insn->opcode()), "block %d in %s\n",
                          id(), SHOW(*m_parent));
        result = mie.target->case_key;
      }
      m_entries.erase_and_dispose(it);
      return result;
    }
  }
  always_assert_log(false,
                    "block %d has no targets matching %s:\n%s",
                    id(),
                    SHOW(branch->insn),
                    SHOW(&m_entries));
  not_reached();
}

std::ostream& operator<<(std::ostream& os, const Edge& e) {
  switch (e.type()) {
  case EDGE_GOTO: {
    os << "goto";
    break;
  }
  case EDGE_BRANCH: {
    os << "branch";
    auto key = e.case_key();
    if (key) {
      os << " " << *key;
    }
    break;
  }
  case EDGE_THROW: {
    os << "throw";
    break;
  }
  default: { break; }
  }
  return os;
}

ControlFlowGraph::ControlFlowGraph(IRList* ir, bool editable)
    : m_editable(editable) {
  always_assert_log(ir->size() > 0, "IRList contains no instructions");

  BranchToTargets branch_to_targets;
  TryEnds try_ends;
  TryCatches try_catches;
  std::vector<Block*> exit_blocks;
  Boundaries boundaries; // block boundaries (for editable == true)

  find_block_boundaries(
      ir, branch_to_targets, try_ends, try_catches, boundaries);

  if (m_editable) {
    fill_blocks(ir, boundaries);
  }

  connect_blocks(branch_to_targets);
  add_catch_edges(try_ends, try_catches);
  if (m_editable) {
    remove_try_markers();
  }

  if (m_editable) {
    TRACE(CFG, 5, "before simplify:\n%s", SHOW(*this));
    simplify();
    TRACE(CFG, 5, "after simplify:\n%s", SHOW(*this));
  } else {
    remove_unreachable_succ_edges();
  }

  sanity_check();
  TRACE(CFG, 5, "editable %d, %s", m_editable, SHOW(*this));
}

void ControlFlowGraph::find_block_boundaries(IRList* ir,
                                             BranchToTargets& branch_to_targets,
                                             TryEnds& try_ends,
                                             TryCatches& try_catches,
                                             Boundaries& boundaries) {
  // Find the block boundaries
  auto* block = create_block();
  if (m_editable) {
    boundaries[block].first = ir->begin();
  } else {
    block->m_begin = ir->begin();
  }

  set_entry_block(block);
  // The first block can be a branch target.
  auto begin = ir->begin();
  if (begin->type == MFLOW_TARGET) {
    branch_to_targets[begin->target->src].push_back(block);
  }
  MethodItemEntry* active_try = nullptr;
  for (auto it = ir->begin(); it != ir->end(); ++it) {
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
    if (!end_of_block(ir, it, active_try != nullptr)) {
      continue;
    }

    // End the current block.
    auto next = std::next(it);
    if (m_editable) {
      boundaries[block].second = next;
    } else {
      block->m_end = next;
    }

    if (next == ir->end()) {
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
      } while (++next != ir->end() && next->type == MFLOW_TARGET);
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
      } while (++next != ir->end() && next->type == MFLOW_CATCH);
      it = std::prev(next, 2);
    }
  }
  TRACE(CFG, 5, "  build: boundaries found\n");
}

// Link the blocks together with edges. If the CFG is editable, also insert
// fallthrough goto instructions and delete MFLOW_TARGETs.
void ControlFlowGraph::connect_blocks(BranchToTargets& branch_to_targets) {
  for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
    // Set outgoing edge if last MIE falls through
    Block* b = it->second;
    auto& last_mie = *b->rbegin();
    bool fallthrough = true;
    if (last_mie.type == MFLOW_OPCODE) {
      auto last_op = last_mie.insn->opcode();
      if (is_branch(last_op)) {
        fallthrough = !is_goto(last_op);
        auto const& target_blocks = branch_to_targets[&last_mie];

        for (auto target_block : target_blocks) {
          if (m_editable) {
            // The the branch information is stored in the edges, we don't need
            // the targets inside the blocks anymore
            auto case_key =
                target_block->remove_first_matching_target(&last_mie);
            if (case_key != boost::none) {
              add_edge(b, target_block, *case_key);
              continue;
            }
          }
          auto edge_type = is_goto(last_op) ? EDGE_GOTO : EDGE_BRANCH;
          add_edge(b, target_block, edge_type);
        }

        if (m_editable && is_goto(last_op)) {
          // We don't need the gotos in editable mode because the edges
          // fully encode that information
          b->m_entries.erase_and_dispose(
              b->m_entries.iterator_to(last_mie));
        }

      } else if (is_return(last_op) || last_op == OPCODE_THROW) {
        fallthrough = false;
      }
    }

    auto next = std::next(it);
    Block* next_b = next->second;
    if (fallthrough && next != m_blocks.end()) {
      TRACE(CFG,
            6,
            "setting default successor %d -> %d\n",
            b->id(),
            next_b->id());
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
      if (block_begin != block->end() && block_begin->type == MFLOW_TRY) {
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

// Move the `MethodItemEntry`s from `ir` into the blocks, based on the
// information in `boundaries`.
//
// The CFG takes ownership of the `MethodItemEntry`s and `ir` is left empty.
void ControlFlowGraph::fill_blocks(IRList* ir, const Boundaries& boundaries) {
  always_assert(m_editable);
  // fill the blocks between their boundaries
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    b->m_entries.splice_selection(b->m_entries.end(),
                                  *ir,
                                  boundaries.at(b).first,
                                  boundaries.at(b).second);
    always_assert_log(!b->empty(), "block %d is empty:\n%s\n", entry.first,
                      SHOW(*this));
  }
  TRACE(CFG, 5, "  build: splicing finished\n");
}

void ControlFlowGraph::simplify() {
  // remove empty blocks
  for (auto it = m_blocks.begin(); it != m_blocks.end();) {
    Block* b = it->second;
    const auto& succs = b->succs();
    if (b->empty() && succs.size() > 0) {
      always_assert_log(succs.size() == 1,
        "too many successors for empty block %d:\n%s", it->first, SHOW(*this));
      const auto& succ_edge = succs[0];
      Block* succ = succ_edge->target();

      if (b == succ || // `b` follows itself: an infinite loop
          b == entry_block()) { // can't redirect nonexistent predecessors
        ++it;
        continue;
      }

      remove_all_edges(b, succ);

      // redirect from my predecessors to my successor (skipping this block)
      // Can't move edges around while we iterate through the edge list
      std::vector<std::shared_ptr<Edge>> need_redirect(b->m_preds.begin(),
                                                       b->m_preds.end());
      for (auto pred_edge : need_redirect) {
        redirect_edge(pred_edge, succ);
      }
    }

    if (b->empty()) {
      it = m_blocks.erase(it);
    } else {
      ++it;
    }
  }
}

// Verify that
//  * MFLOW_TARGETs are gone
//  * OPCODE_GOTOs are gone
//  * Correct number of outgoing edges
void ControlFlowGraph::sanity_check() {
  if (m_editable) {
    for (const auto& entry : m_blocks) {
      Block* b = entry.second;
      for (const auto& mie : *b) {
        always_assert_log(mie.type != MFLOW_TARGET,
                          "failed to remove all targets. block %d in\n%s",
                          b->id(), SHOW(*this));
        if (mie.type == MFLOW_OPCODE) {
          always_assert_log(!is_goto(mie.insn->opcode()),
                            "failed to remove all gotos. block %d in\n%s",
                            b->id(), SHOW(*this));
        }
      }

      auto last_it = b->get_last_insn();
      if (last_it != b->end()) {
        auto& last_mie = *last_it;
        if (last_mie.type == MFLOW_OPCODE) {
          size_t num_succs = b->succs().size();
          auto op = last_mie.insn->opcode();
          if (is_conditional_branch(op) || is_switch(op)) {
            always_assert_log(num_succs > 1, "block %d, %s", b->id(), SHOW(*this));
          } else if (is_return(op)) {
            always_assert_log(num_succs == 0, "block %d, %s", b->id(), SHOW(*this));
          } else if (is_throw(op)) {
            // A throw could end the method or go to a catch handler.
            // We don't have any useful assertions to make here.
          } else {
            always_assert_log(num_succs > 0, "block %d, %s", b->id(), SHOW(*this));
          }
        }
      }
    }
  }

  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    // make sure the edge list in both blocks agree
    for (const auto e : b->succs()) {
      const auto& reverse_edges = e->target()->preds();
      always_assert_log(std::find(reverse_edges.begin(), reverse_edges.end(),
                                  e) != reverse_edges.end(),
                        "block %d -> %d, %s", b->id(), e->target()->id(),
                        SHOW(*this));
    }
    for (const auto e : b->preds()) {
      const auto& forward_edges = e->src()->succs();
      always_assert_log(std::find(forward_edges.begin(), forward_edges.end(),
                                  e) != forward_edges.end(),
                        "block %d -> %d, %s", e->src()->id(), b->id(),
                        SHOW(*this));
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

  std::vector<Block*> ordering;
  std::set<BlockId> finished_blocks;

  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    if (finished_blocks.count(b->id()) != 0) {
      continue;
    }

    ordering.push_back(b);
    finished_blocks.insert(b->id());

    // If the GOTO edge leads to a block with a move-result(-pseudo), then that
    // block must be placed immediately after this one because we can't insert
    // anything between an instruction and its move-result(-pseudo).
    auto goto_edge = get_succ_edge_if(
        b, [](const std::shared_ptr<Edge>& e) { return e->type() == EDGE_GOTO; });
    if (goto_edge != nullptr) {
      auto goto_block = goto_edge->target();
      auto first_it = goto_block->get_first_insn();
      if (first_it != goto_block->end()) {
        auto first_op = first_it->insn->opcode();
        if ((is_move_result(first_op) ||
             opcode::is_move_result_pseudo(first_op)) &&
            finished_blocks.count(goto_block->id()) == 0) {
          ordering.push_back(goto_block);
          finished_blocks.insert(goto_block->id());
        }
      }
    }
  }
  return ordering;
}

// Add an MFLOW_TARGET at the end of each edge.
// Insert GOTOs where necessary.
void ControlFlowGraph::insert_branches_and_targets(
    const std::vector<Block*>& ordering) {
  for (auto it = ordering.begin(); it != ordering.end(); ++it) {
    Block* b = *it;

    for (const auto& edge : b->succs()) {
      if (edge->type() == EDGE_BRANCH) {
        auto branch_it = b->get_conditional_branch();
        always_assert_log(branch_it != b->end(), "block %d %s", b->id(), SHOW(*this));
        auto& branch_mie = *branch_it;

        BranchTarget* bt = edge->m_case_key != boost::none
                               ? new BranchTarget(&branch_mie, *edge->m_case_key)
                               : new BranchTarget(&branch_mie);
        auto target_mie = new MethodItemEntry(bt);
        edge->target()->m_entries.push_front(*target_mie);

      } else if (edge->type() == EDGE_GOTO) {
        auto next_it = std::next(it);
        if (next_it != ordering.end()) {
          Block* next = *next_it;
          if (edge->target() == next) {
            // Don't need a goto because this will fall through to `next`
            continue;
          }
        }
        auto branch_mie = new MethodItemEntry(new IRInstruction(OPCODE_GOTO));
        auto target_mie = new MethodItemEntry(new BranchTarget(branch_mie));
        edge->src()->m_entries.push_back(*branch_mie);
        edge->target()->m_entries.push_front(*target_mie);
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
        });
  }
}

IRList* ControlFlowGraph::linearize() {
  IRList* result = new IRList;

  TRACE(CFG, 5, "before linearize:\n%s", SHOW(*this));
  simplify();
  sanity_check();

  const std::vector<Block*>& ordering = order();
  insert_branches_and_targets(ordering);
  insert_try_markers(ordering);

  for (Block* b : ordering) {
    result->splice(result->end(), b->m_entries);
  }

  return result;
}

void ControlFlowGraph::insert_try_markers(const std::vector<Block*>& ordering) {
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

template <class... Args>
void ControlFlowGraph::add_edge(Args&&... args) {
  auto edge = std::make_shared<Edge>(std::forward<Args>(args)...);
  edge->src()->m_succs.emplace_back(edge);
  edge->target()->m_preds.emplace_back(edge);
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

void ControlFlowGraph::remove_edge(std::shared_ptr<Edge> edge) {
  remove_edge_if(
      edge->src(), edge->target(),
      [edge](const std::shared_ptr<Edge>& e) { return edge == e; });
}

void ControlFlowGraph::remove_edge_if(
    Block* source,
    Block* target,
    const EdgePredicate& predicate) {

  auto& forward_edges = source->m_succs;
  std::unordered_set<std::shared_ptr<Edge>> to_remove;
  forward_edges.erase(
      std::remove_if(forward_edges.begin(),
                     forward_edges.end(),
                     [&target, &predicate, &to_remove](const std::shared_ptr<Edge>& e) {
                       if (e->target() == target && predicate(e)) {
                         to_remove.insert(e);
                         return true;
                       }
                       return false;
                     }),
      forward_edges.end());

  auto& reverse_edges = target->m_preds;
  reverse_edges.erase(
      std::remove_if(reverse_edges.begin(),
                     reverse_edges.end(),
                     [&to_remove](const std::shared_ptr<Edge>& e) {
                       return to_remove.count(e) > 0;
                     }),
      reverse_edges.end());
}

void ControlFlowGraph::remove_pred_edge_if(Block* block,
                                           const EdgePredicate& predicate) {
  auto& reverse_edges = block->m_preds;

  std::vector<Block*> source_blocks;
  std::unordered_set<std::shared_ptr<Edge>> to_remove;
  reverse_edges.erase(std::remove_if(reverse_edges.begin(),
                             reverse_edges.end(),
                             [&source_blocks, &to_remove,
                              &predicate](const std::shared_ptr<Edge>& e) {
                               if (predicate(e)) {
                                 source_blocks.push_back(e->src());
                                 to_remove.insert(e);
                                 return true;
                               }
                               return false;
                             }),
              reverse_edges.end());

  for (Block* source_block : source_blocks) {
    auto& forward_edges = source_block->m_succs;
    forward_edges.erase(std::remove_if(forward_edges.begin(), forward_edges.end(),
                                 [&to_remove](const std::shared_ptr<Edge>& e) {
                                   return to_remove.count(e) > 0;
                                 }),
                  forward_edges.end());
  }
}

void ControlFlowGraph::remove_succ_edge_if(Block* block,
                                           const EdgePredicate& predicate) {

  auto& forward_edges = block->m_succs;

  std::vector<Block*> target_blocks;
  std::unordered_set<std::shared_ptr<Edge>> to_remove;
  forward_edges.erase(std::remove_if(forward_edges.begin(),
                             forward_edges.end(),
                             [&target_blocks, &to_remove,
                              &predicate](const std::shared_ptr<Edge>& e) {
                               if (predicate(e)) {
                                 target_blocks.push_back(e->target());
                                 to_remove.insert(e);
                                 return true;
                               }
                               return false;
                             }),
              forward_edges.end());

  for (Block* target_block : target_blocks) {
    auto& reverse_edges = target_block->m_preds;
    reverse_edges.erase(std::remove_if(reverse_edges.begin(), reverse_edges.end(),
                                 [&to_remove](const std::shared_ptr<Edge>& e) {
                                   return to_remove.count(e) > 0;
                                 }),
                  reverse_edges.end());
  }
}

std::shared_ptr<Edge> ControlFlowGraph::get_pred_edge_if(
    Block* block, const EdgePredicate& predicate) {
  for (auto e : block->preds()) {
    if (predicate(e)) {
      return e;
    }
  }
  return nullptr;
}

std::shared_ptr<Edge> ControlFlowGraph::get_succ_edge_if(
    Block* block, const EdgePredicate& predicate) {
  for (auto e : block->succs()) {
    if (predicate(e)) {
      return e;
    }
  }
  return nullptr;
}

// Move this edge out of the vectors between its old blocks
// and into the vectors between the new blocks
void ControlFlowGraph::redirect_edge(std::shared_ptr<Edge> edge,
                                     Block* new_target) {
  remove_edge(edge);
  edge->m_target = new_target;
  edge->src()->m_succs.push_back(edge);
  edge->target()->m_preds.push_back(edge);
}

void ControlFlowGraph::remove_opcode(const InstructionIterator& it) {
  always_assert(m_editable);

  MethodItemEntry& mie = *it;
  auto insn = mie.insn;
  auto op = insn->opcode();
  Block* block = it.block();

  if (is_conditional_branch(op) || is_switch(op)) {
    // Remove all outgoing EDGE_BRANCHes
    // leaving behind only an EDGE_GOTO (and maybe an EDGE_THROW?)
    remove_succ_edge_if(block, [](std::shared_ptr<Edge> e) {
      return e->type() == EDGE_BRANCH;
    });
    block->m_entries.erase_and_dispose(it.unwrap());
  } else if (is_goto(op)) {
    always_assert_log(false, "There are no GOTO instructions in the CFG");
  } else {
    block->remove_opcode(it.unwrap());
  }

  sanity_check();
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
    const IRList::iterator& loc) const {
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
  remove_succ_edge_if(b, [](const std::shared_ptr<Edge>&) { return true; });
}

void ControlFlowGraph::remove_pred_edges(Block* b) {
  remove_pred_edge_if(b, [](const std::shared_ptr<Edge>&) { return true; });
}

} // namespace cfg
