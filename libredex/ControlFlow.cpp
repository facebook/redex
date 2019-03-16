/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ControlFlow.h"

#include <boost/dynamic_bitset.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <iterator>
#include <stack>
#include <utility>

#include "DexUtil.h"
#include "Transform.h"
#include "WeakTopologicalOrdering.h"

namespace {

// return true if `it` should be the last instruction of this block
bool end_of_block(const IRList* ir, IRList::iterator it, bool in_try) {
  auto next = std::next(it);
  if (next == ir->end()) {
    return true;
  }

  // End the block before the first target in a contiguous sequence of targets.
  if (next->type == MFLOW_TARGET && it->type != MFLOW_TARGET) {
    return true;
  }

  // End the block before the first catch marker in a contiguous sequence of
  // catch markers.
  if (next->type == MFLOW_CATCH && it->type != MFLOW_CATCH) {
    return true;
  }

  // End the block before a TRY_START
  // and after a TRY_END
  if ((next->type == MFLOW_TRY && next->tentry->type == TRY_START) ||
      (it->type == MFLOW_TRY && it->tentry->type == TRY_END)) {
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
    return opcode::can_throw(last->insn->opcode());
  }
  return false;
}

bool cannot_throw(cfg::Block* b) {
  for (const auto& mie : InstructionIterable(b)) {
    auto op = mie.insn->opcode();
    if (opcode::can_throw(op)) {
      return false;
    }
  }
  return true;
}

/*
 * Return true if this is empty or only has positions in it
 */
bool is_effectively_empty(cfg::Block* b) {
  for (const auto& mie : *b) {
    if (mie.type != MFLOW_POSITION) {
      return false;
    }
  }
  return true;
}

/*
 * Return an iterator to the first instruction (except move-result* and goto) if
 * it occurs before the first position. Otherwise return end.
 */
IRList::iterator insn_before_position(cfg::Block* b) {
  for (auto it = b->begin(); it != b->end(); ++it) {
    if (it->type == MFLOW_OPCODE) {
      auto op = it->insn->opcode();
      if (!is_move_result(op) && !opcode::is_move_result_pseudo(op) &&
          !is_goto(op)) {
        return it;
      }
    } else if (it->type == MFLOW_POSITION) {
      return b->end();
    }
  }
  return b->end();
}

/*
 * Given an output ordering, Find adjacent positions that are exact duplicates
 * and delete the extras. Make sure not to delete any positions that are
 * referenced by a parent pointer.
 */
void remove_duplicate_positions(IRList* ir) {
  std::unordered_set<DexPosition*> keep;
  for (auto& mie : *ir) {
    if (mie.type == MFLOW_POSITION && mie.pos->parent != nullptr) {
      keep.insert(mie.pos->parent);
    }
  }
  DexPosition* prev = nullptr;
  for (auto it = ir->begin(); it != ir->end();) {
    if (it->type == MFLOW_POSITION) {
      DexPosition* curr = it->pos.get();
      if (prev != nullptr && *curr == *prev && keep.count(curr) == 0) {
        it = ir->erase_and_dispose(it);
        continue;
      } else {
        prev = curr;
      }
    }
    ++it;
  }
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

bool Block::is_catch() const {
  return m_parent->get_pred_edge_of_type(this, EDGE_THROW) != nullptr;
}

bool Block::same_try(const Block* other) const {
  always_assert(other->m_parent == this->m_parent);
  return m_parent->blocks_are_in_same_try(this, other);
}

void Block::remove_opcode(const ir_list::InstructionIterator& it) {
  always_assert(m_parent->editable());
  m_parent->remove_opcode(cfg::InstructionIterator(*m_parent, this, it));
}

void Block::remove_opcode(const IRList::iterator& it) {
  always_assert(m_parent->editable());
  remove_opcode(ir_list::InstructionIterator(it, end()));
}

opcode::Branchingness Block::branchingness() {
  always_assert(m_parent->editable());
  const auto& last = get_last_insn();

  if (succs().empty() ||
      (succs().size() == 1 &&
       m_parent->get_succ_edge_of_type(this, EDGE_GHOST) != nullptr)) {
    if (last != end()) {
      auto op = last->insn->opcode();
      if (is_return(op)) {
        return opcode::BRANCH_RETURN;
      } else if (op == OPCODE_THROW) {
        return opcode::BRANCH_THROW;
      }
    }
    return opcode::BRANCH_NONE;
  }

  if (m_parent->get_succ_edge_of_type(this, EDGE_THROW) != nullptr) {
    return opcode::BRANCH_THROW;
  }

  if (m_parent->get_succ_edge_of_type(this, EDGE_BRANCH) != nullptr) {
    always_assert(last != end());
    auto br = opcode::branchingness(last->insn->opcode());
    always_assert(br == opcode::BRANCH_IF || br == opcode::BRANCH_SWITCH);
    return br;
  }

  if (m_parent->get_succ_edge_of_type(this, EDGE_GOTO) != nullptr) {
    return opcode::BRANCH_GOTO;
  }
  return opcode::BRANCH_NONE;
}

uint32_t Block::num_opcodes() const {
  always_assert(m_parent->editable());
  return m_entries.count_opcodes();
}

uint32_t Block::sum_opcode_sizes() const {
  always_assert(m_parent->editable());
  return m_entries.sum_opcode_sizes();
}

// shallowly copy pointers (edges and parent cfg)
// but deeply copy MethodItemEntries
Block::Block(const Block& b, MethodItemEntryCloner* cloner)
    : m_id(b.m_id),
      m_preds(b.m_preds),
      m_succs(b.m_succs),
      m_parent(b.m_parent) {

  // only for editable, don't worry about m_begin and m_end
  always_assert(m_parent->editable());

  for (const auto& mie : b.m_entries) {
    m_entries.push_back(*cloner->clone(&mie));
  }
}

bool Block::has_pred(Block* b, EdgeType t) const {
  const auto& edges = preds();
  return std::find_if(edges.begin(), edges.end(), [b, t](const Edge* edge) {
           return edge->src() == b &&
                  (t == EDGE_TYPE_SIZE || edge->type() == t);
         }) != edges.end();
}

bool Block::has_succ(Block* b, EdgeType t) const {
  const auto& edges = succs();
  return std::find_if(edges.begin(), edges.end(), [b, t](const Edge* edge) {
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

bool Block::starts_with_move_result() {
  auto first_it = get_first_insn();
  if (first_it != end()) {
    auto first_op = first_it->insn->opcode();
    if (is_move_result(first_op) || opcode::is_move_result_pseudo(first_op)) {
      return true;
    }
  }
  return false;
}

Block* Block::follow_goto() const {
  const auto& s = succs();
  if (s.size() == 1) {
    const auto& e = s[0];
    if (e->type() == EDGE_GOTO) {
      return e->target();
    }
  }
  return nullptr;
}

std::vector<Edge*> Block::get_outgoing_throws_in_order() const {
  std::vector<Edge*> result =
      m_parent->get_succ_edges_of_type(this, EDGE_THROW);
  std::sort(result.begin(), result.end(), [](const Edge* e1, const Edge* e2) {
    return e1->m_throw_info->index < e2->m_throw_info->index;
  });
  return result;
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

// These assume that the iterator is inside this block
cfg::InstructionIterator Block::to_cfg_instruction_iterator(
    const ir_list::InstructionIterator& list_it) {
  if (ControlFlowGraph::DEBUG && list_it.unwrap() != end()) {
    bool inside = false;
    auto needle = list_it.unwrap();
    for (auto it = begin(); it != end(); ++it) {
      if (it == needle) {
        inside = true;
      }
    }
    always_assert(inside);
  }
  return cfg::InstructionIterator(*m_parent, this, list_it);
}

cfg::InstructionIterator Block::to_cfg_instruction_iterator(
    const IRList::iterator& list_it) {
  always_assert(list_it == this->end() || list_it->type == MFLOW_OPCODE);
  return to_cfg_instruction_iterator(
      ir_list::InstructionIterator(list_it, this->end()));
}

cfg::InstructionIterator Block::to_cfg_instruction_iterator(
    MethodItemEntry& mie) {
  always_assert(m_parent->editable());
  return to_cfg_instruction_iterator(m_entries.iterator_to(mie));
}

// Forward the insertion methods to the parent CFG.
bool Block::insert_before(const InstructionIterator& position,
                          const std::vector<IRInstruction*>& insns) {
  always_assert(position.block() == this);
  return m_parent->insert_before(position, insns);
}
bool Block::insert_before(const InstructionIterator& position,
                          IRInstruction* insn) {
  always_assert(position.block() == this);
  return m_parent->insert_before(position, insn);
}
bool Block::insert_after(const InstructionIterator& position,
                         const std::vector<IRInstruction*>& insns) {
  always_assert(position.block() == this);
  return m_parent->insert_after(position, insns);
}
bool Block::insert_after(const InstructionIterator& position,
                         IRInstruction* insn) {
  always_assert(position.block() == this);
  return m_parent->insert_after(position, insn);
}
bool Block::push_front(const std::vector<IRInstruction*>& insns) {
  return m_parent->push_front(this, insns);
}
bool Block::push_front(IRInstruction* insn) {
  return m_parent->push_front(this, insn);
}
bool Block::push_back(const std::vector<IRInstruction*>& insns) {
  return m_parent->push_back(this, insns);
}
bool Block::push_back(IRInstruction* insn) {
  return m_parent->push_back(this, insn);
}

std::ostream& operator<<(std::ostream& os, const Edge& e) {
  switch (e.type()) {
  case EDGE_GOTO: {
    os << "goto";
    break;
  }
  case EDGE_BRANCH: {
    os << "branch";
    const auto& key = e.case_key();
    if (key) {
      os << " " << *key;
    }
    break;
  }
  case EDGE_THROW: {
    os << "throw";
    break;
  }
  default: {
    break;
  }
  }
  return os;
}

ControlFlowGraph::ControlFlowGraph(IRList* ir,
                                   uint16_t registers_size,
                                   bool editable)
    : m_registers_size(registers_size), m_editable(editable) {
  always_assert_log(ir->size() > 0, "IRList contains no instructions");

  BranchToTargets branch_to_targets;
  TryEnds try_ends;
  TryCatches try_catches;

  find_block_boundaries(ir, branch_to_targets, try_ends, try_catches);

  connect_blocks(branch_to_targets);
  add_catch_edges(try_ends, try_catches);

  if (m_editable) {
    remove_try_catch_markers();

    // Often, the `registers_size` parameter passed into this constructor is
    // incorrect. We recompute here to safeguard against this.
    // TODO: fix the optimizations that don't track registers size correctly.
    recompute_registers_size();

    TRACE(CFG, 5, "before simplify:\n%s", SHOW(*this));
    simplify();
    TRACE(CFG, 5, "after simplify:\n%s", SHOW(*this));
  } else {
    remove_unreachable_succ_edges();
  }

  TRACE(CFG, 5, "editable %d, %s", m_editable, SHOW(*this));
}

void ControlFlowGraph::find_block_boundaries(IRList* ir,
                                             BranchToTargets& branch_to_targets,
                                             TryEnds& try_ends,
                                             TryCatches& try_catches) {
  // create the entry block
  auto* block = create_block();
  IRList::iterator block_begin;
  if (m_editable) {
    block_begin = ir->begin();
  } else {
    block->m_begin = ir->begin();
  }
  set_entry_block(block);

  bool in_try = false;
  IRList::iterator next;
  DexPosition* current_position = nullptr;
  DexPosition* last_pos_before_this_block = nullptr;
  for (auto it = ir->begin(); it != ir->end(); it = next) {
    next = std::next(it);
    if (it->type == MFLOW_TRY) {
      if (it->tentry->type == TRY_START) {
        // Assumption: TRY_STARTs are only at the beginning of blocks
        always_assert(!m_editable || it == block_begin);
        always_assert(m_editable || it == block->m_begin);
        in_try = true;
      } else if (it->tentry->type == TRY_END) {
        try_ends.emplace_back(it->tentry, block);
        in_try = false;
      }
    } else if (it->type == MFLOW_CATCH) {
      try_catches[it->centry] = block;
    } else if (it->type == MFLOW_TARGET) {
      branch_to_targets[it->target->src].push_back(block);
    } else if (it->type == MFLOW_POSITION) {
      current_position = it->pos.get();
    }

    if (!end_of_block(ir, it, in_try)) {
      continue;
    }

    // End the current block.
    if (m_editable) {
      // Steal the code from the ir and put it into the block.
      // This is safe to do while iterating in ir because iterators in ir now
      // point to elements of block->m_entries (and we already computed next).
      block->m_entries.splice_selection(block->m_entries.end(), *ir,
                                        block_begin, next);
      if (last_pos_before_this_block != nullptr) {
        auto first_insn = insn_before_position(block);
        if (first_insn != block->end()) {
          // DexPositions apply to every instruction in the linear stream until
          // the next DexPosition. Because we're breaking up the linear stream
          // into many small blocks, we need to make sure that instructions stay
          // associated with the same DexPosition as they were in the input
          // IRList.
          //
          // This creates duplicate positions, but we will remove any extras at
          // linearize time.
          block->m_entries.insert_before(
              first_insn,
              std::make_unique<DexPosition>(*last_pos_before_this_block));
        }
      }
    } else {
      block->m_end = next;
    }

    if (next == ir->end()) {
      break;
    }

    // Start a new block at the next MethodItem.
    block = create_block();
    if (m_editable) {
      last_pos_before_this_block = current_position;
      block_begin = next;
    } else {
      block->m_begin = next;
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
          b->m_entries.erase_and_dispose(b->m_entries.iterator_to(last_mie));
        }

      } else if (is_return(last_op) || last_op == OPCODE_THROW) {
        fallthrough = false;
      }
    }

    auto next = std::next(it);
    Block* next_b = next->second;
    if (fallthrough && next != m_blocks.end()) {
      TRACE(CFG, 6, "adding fallthrough goto %d -> %d\n", b->id(),
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
    while (true) {
      Block* block = m_blocks.at(bid);
      if (ends_with_may_throw(block)) {
        uint32_t i = 0;
        for (auto mie = try_end->catch_start; mie != nullptr;
             mie = mie->centry->next) {
          auto catchblock = try_catches.at(mie->centry);
          // Create a throw edge with the information from this catch entry
          add_edge(block, catchblock, mie->centry->catch_type, i);
          ++i;
        }
      }
      auto block_begin = block->begin();
      if (block_begin != block->end() && block_begin->type == MFLOW_TRY) {
        auto tentry = block_begin->tentry;
        if (tentry->type == TRY_START) {
          always_assert_log(tentry->catch_start == try_end->catch_start, "%s",
                            SHOW(*this));
          break;
        }
      }
      always_assert_log(bid > 0, "No beginning of try region found");
      --bid;
    }
  }
  TRACE(CFG, 5, "  build: catch edges added\n");
}

BlockId ControlFlowGraph::next_block_id() const {
  // Choose the next largest id. Note that we can't use m_block.size() because
  // we may have deleted some blocks from the cfg.
  const auto& rbegin = m_blocks.rbegin();
  return (rbegin == m_blocks.rend()) ? 0 : (rbegin->first + 1);
}

void ControlFlowGraph::remove_unreachable_succ_edges() {
  // Remove edges between unreachable blocks and their succ blocks.
  if (m_blocks.empty()) {
    return;
  }

  const auto& visited = visit();
  if (visited.all()) {
    // All blocks are visited. No blocks need to have their succ edges removed.
    return;
  }

  for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
    Block* b = it->second;
    if (visited.test(b->id())) {
      continue;
    }

    TRACE(CFG, 5, "  build: removing succ edges from block %d\n", b->id());
    delete_succ_edges(b);
  }
  TRACE(CFG, 5, "  build: unreachables removed\n");
}

/*
 * Traverse the graph, starting from the entry node. Return a bitset with IDs of
 * reachable blocks having 1 and IDs of unreachable blocks (or unused IDs)
 * having 0.
 */
boost::dynamic_bitset<> ControlFlowGraph::visit() const {
  std::stack<const cfg::Block*> to_visit;
  boost::dynamic_bitset<> visited{next_block_id()};
  to_visit.push(entry_block());
  while (!to_visit.empty()) {
    const cfg::Block* b = to_visit.top();
    to_visit.pop();

    if (visited.test_set(b->id())) {
      continue;
    }

    for (Edge* e : b->succs()) {
      to_visit.push(e->target());
    }
  }
  return visited;
}

void ControlFlowGraph::simplify() {
  remove_unreachable_blocks();
  remove_empty_blocks();
}

// remove blocks with no predecessors
uint32_t ControlFlowGraph::remove_unreachable_blocks() {
  uint32_t num_insns_removed = 0;
  remove_unreachable_succ_edges();
  std::unordered_set<DexPosition*> deleted_positions;
  bool need_register_size_fix = false;
  for (auto it = m_blocks.begin(); it != m_blocks.end();) {
    Block* b = it->second;
    const auto& preds = b->preds();
    if (preds.empty() && b != entry_block()) {
      for (const auto& mie : *b) {
        if (mie.type == MFLOW_POSITION) {
          deleted_positions.insert(mie.pos.get());
        } else if (mie.type == MFLOW_OPCODE) {
          auto insn = mie.insn;
          if (insn->dests_size()) {
            // +1 because registers start at zero
            auto size_required = insn->dest() + insn->dest_is_wide() + 1;
            if (size_required >= m_registers_size) {
              // We're deleting an instruction that may have been the max
              // register of the entire function.
              need_register_size_fix = true;
            }
          }
        }
      }
      num_insns_removed += b->num_opcodes();
      always_assert(b->succs().empty());
      always_assert(b->preds().empty());
      delete b;
      it = m_blocks.erase(it);
    } else {
      ++it;
    }
  }

  if (need_register_size_fix) {
    recompute_registers_size();
  }
  remove_dangling_parents(deleted_positions);

  return num_insns_removed;
}

void ControlFlowGraph::remove_dangling_parents(
    const std::unordered_set<DexPosition*>& deleted_positions) {
  // We don't want to leave any dangling dex parent pointers behind
  if (!deleted_positions.empty()) {
    for (const auto& entry : m_blocks) {
      for (const auto& mie : *entry.second) {
        if (mie.type == MFLOW_POSITION && mie.pos->parent != nullptr &&
            deleted_positions.count(mie.pos->parent)) {
          mie.pos->parent = nullptr;
        }
      }
    }
  }
}
void ControlFlowGraph::remove_empty_blocks() {
  always_assert(editable());
  std::unordered_set<DexPosition*> deleted_positions;
  for (auto it = m_blocks.begin(); it != m_blocks.end();) {
    Block* b = it->second;
    if (!is_effectively_empty(b) || b == exit_block()) {
      ++it;
      continue;
    }

    const auto& succs = get_succ_edges_if(
        b, [](const Edge* e) { return e->type() != EDGE_GHOST; });
    if (succs.size() > 0) {
      always_assert_log(succs.size() == 1,
                        "too many successors for empty block %d:\n%s",
                        it->first, SHOW(*this));
      const auto& succ_edge = succs[0];
      Block* succ = succ_edge->target();

      if (b == succ) { // `b` follows itself: an infinite loop
        ++it;
        continue;
      }
      // b is empty. Reorganize the edges so we can remove it

      // Remove the one goto edge from b to succ
      delete_edges_between(b, succ);

      // If b was a predecessor of the exit block (for example, part of an
      // infinite loop) we need to transfer that info to `succ` because `b` will
      // be made unreachable and deleted by simplify
      auto ghost = get_succ_edge_of_type(b, EDGE_GHOST);
      if (ghost != nullptr) {
        set_edge_source(ghost, succ);
      }

      // Redirect from b's predecessors to b's successor (skipping b). We
      // can't move edges around while we iterate through the edge list
      // though.
      std::vector<Edge*> need_redirect(b->m_preds.begin(), b->m_preds.end());
      for (Edge* pred_edge : need_redirect) {
        set_edge_target(pred_edge, succ);
      }

      if (b == entry_block()) {
        m_entry_block = succ;
      }
    }

    for (const auto& mie : *b) {
      if (mie.type == MFLOW_POSITION) {
        deleted_positions.insert(mie.pos.get());
      }
    }
    delete b;
    it = m_blocks.erase(it);
  }
  remove_dangling_parents(deleted_positions);
}

void ControlFlowGraph::no_unreferenced_edges() const {
  EdgeSet referenced;
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    for (Edge* e : b->preds()) {
      referenced.insert(e);
    }
    for (Edge* e : b->succs()) {
      referenced.insert(e);
    }
  }
  always_assert(referenced == m_edges);
}

// Verify that
//  * MFLOW_TARGETs are gone
//  * OPCODE_GOTOs are gone
//  * Correct number of outgoing edges
void ControlFlowGraph::sanity_check() const {
  if (m_editable) {
    for (const auto& entry : m_blocks) {
      Block* b = entry.second;
      if (DEBUG) {
        // No targets or gotos
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
      }

      // Last instruction matches outgoing edges
      auto num_goto_succs = get_succ_edges_of_type(b, EDGE_GOTO).size();
      auto last_it = b->get_last_insn();
      auto num_preds = b->preds().size();
      auto num_succs =
          get_succ_edges_if(
              b, [](const Edge* e) { return e->type() != EDGE_GHOST; })
              .size();
      if (last_it != b->end()) {
        auto op = last_it->insn->opcode();

        if (is_conditional_branch(op)) {
          always_assert_log(num_succs == 2, "block %d, %s", b->id(),
                            SHOW(*this));
        } else if (is_switch(op)) {
          always_assert_log(num_succs > 1, "block %d, %s", b->id(),
                            SHOW(*this));
        } else if (is_return(op)) {
          // Make sure we don't have any outgoing edges (except EDGE_GHOST)
          always_assert_log(num_succs == 0, "block %d, %s", b->id(),
                            SHOW(*this));
        } else if (is_throw(op)) {
          // A throw could end the method or go to a catch handler.
          // Make sure this block has no outgoing non-throwing edges
          auto non_throw_edge = get_succ_edge_if(b, [](const Edge* e) {
            return e->type() != EDGE_THROW && e->type() != EDGE_GHOST;
          });
          always_assert_log(non_throw_edge == nullptr, "block %d, %s", b->id(),
                            SHOW(*this));
        }

        if (num_preds > 0 && !(is_return(op) || is_throw(op))) {
          // Control Flow shouldn't just fall off the end of a block, unless
          // it's an orphan block that's unreachable anyway
          always_assert_log(num_succs > 0, "block %d, %s", b->id(),
                            SHOW(*this));
          always_assert_log(num_goto_succs == 1, "block %d, %s", b->id(),
                            SHOW(*this));
        }
      } else if (num_preds > 0 && b != exit_block()) {
        // no instructions in this block. Control Flow shouldn't just fall off
        // the end
        always_assert_log(num_succs > 0, "block %d, %s", b->id(), SHOW(*this));
        always_assert_log(num_goto_succs == 1, "block %d, %s", b->id(),
                          SHOW(*this));
      }

      always_assert_log(num_goto_succs < 2, "block %d, %s", b->id(),
                        SHOW(*this));
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

    const auto& throws = b->get_outgoing_throws_in_order();
    bool last = true;
    // Only the last throw edge can have a null catch type.
    for (auto it = throws.rbegin(); it != throws.rend(); ++it) {
      Edge* e = *it;
      if (!last) {
        always_assert_log(
            e->m_throw_info->catch_type != nullptr,
            "Can't have a catchall (%d -> %d) that isn't last. %s",
            e->src()->id(), e->target()->id(), SHOW(*this));
      }
      last = false;
    }
  }

  if (m_editable) {
    auto used_regs = compute_registers_size();
    always_assert_log(used_regs == m_registers_size,
                      "used regs %d != registers size %d. %s", used_regs,
                      m_registers_size, SHOW(*this));
  }
  no_dangling_dex_positions();
  if (DEBUG) {
    no_unreferenced_edges();
  }
}

uint16_t ControlFlowGraph::compute_registers_size() const {
  uint16_t num_regs = 0;
  for (const auto& mie : cfg::ConstInstructionIterable(*this)) {
    auto insn = mie.insn;
    if (insn->dests_size()) {
      // +1 because registers start at v0
      uint16_t size_required = insn->dest() + insn->dest_is_wide() + 1;
      num_regs = std::max(size_required, num_regs);
    }
  }
  // We don't check the source registers because we shouldn't ever be using an
  // undefined register. If the input code is well-formed, there shouldn't be a
  // source register without an equivalent dest register. This is true for our
  // IR because of the load-param opcodes.
  return num_regs;
}

void ControlFlowGraph::recompute_registers_size() {
  m_registers_size = compute_registers_size();
}

void ControlFlowGraph::no_dangling_dex_positions() const {
  std::unordered_map<DexPosition*, bool> parents;
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    for (const auto& mie : *b) {
      if (mie.type == MFLOW_POSITION && mie.pos->parent != nullptr) {
        parents.emplace(mie.pos->parent, false);
      }
    }
  }

  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    for (const auto& mie : *b) {
      if (mie.type == MFLOW_POSITION) {
        auto search = parents.find(mie.pos.get());
        if (search != parents.end()) {
          search->second = true;
        }
      }
    }
  }

  for (const auto& entry : parents) {
    always_assert_log(entry.second, "%lu is a dangling parent pointer in %s",
                      entry.first, SHOW(*this));
  }
}

uint32_t ControlFlowGraph::num_opcodes() const {
  uint32_t result = 0;
  for (const auto& entry : m_blocks) {
    result += entry.second->num_opcodes();
  }
  return result;
}

uint32_t ControlFlowGraph::sum_opcode_sizes() const {
  uint32_t result = 0;
  for (const auto& entry : m_blocks) {
    result += entry.second->sum_opcode_sizes();
  }
  return result;
}

boost::sub_range<IRList> ControlFlowGraph::get_param_instructions() {
  // Find the first block that has instructions
  Block* block = entry_block();
  while (block->get_first_insn() == block->end()) {
    const auto& succs = block->succs();
    always_assert(succs.size() == 1);
    const auto& out = succs[0];
    always_assert(out->type() == EDGE_GOTO);
    block = out->target();
  }
  return block->m_entries.get_param_instructions();
}

void ControlFlowGraph::gather_catch_types(std::vector<DexType*>& types) const {
  always_assert(editable());
  std::unordered_set<DexType*> seen;
  // get the catch types of all the incoming edges to all the catch blocks
  for (const auto& entry : m_blocks) {
    const Block* b = entry.second;
    if (b->is_catch()) {
      for (const cfg::Edge* e : b->preds()) {
        if (e->type() == cfg::EDGE_THROW) {
          DexType* t = e->throw_info()->catch_type;
          const auto pair = seen.insert(t);
          bool insertion_occured = pair.second;
          if (insertion_occured) {
            types.push_back(t);
          }
        }
      }
    }
  }
}

void ControlFlowGraph::gather_strings(std::vector<DexString*>& strings) const {
  always_assert(editable());
  for (const auto& entry : m_blocks) {
    entry.second->m_entries.gather_strings(strings);
  }
}

void ControlFlowGraph::gather_types(std::vector<DexType*>& types) const {
  always_assert(editable());
  gather_catch_types(types);
  for (const auto& entry : m_blocks) {
    entry.second->m_entries.gather_types(types);
  }
}

void ControlFlowGraph::gather_fields(std::vector<DexFieldRef*>& fields) const {
  always_assert(editable());
  for (const auto& entry : m_blocks) {
    entry.second->m_entries.gather_fields(fields);
  }
}

void ControlFlowGraph::gather_methods(
    std::vector<DexMethodRef*>& methods) const {
  always_assert(editable());
  for (const auto& entry : m_blocks) {
    entry.second->m_entries.gather_methods(methods);
  }
}

cfg::InstructionIterator ControlFlowGraph::move_result_of(
    const cfg::InstructionIterator& it) {
  auto next_insn = std::next(it);
  auto end = cfg::InstructionIterable(*this).end();
  if (next_insn != end && it.block() == next_insn.block()) {
    // The easy case where the move result is in the same block
    auto op = next_insn->insn->opcode();
    if (opcode::is_move_result_pseudo(op) || is_move_result(op)) {
      return next_insn;
    }
  }
  auto goto_edge = get_succ_edge_of_type(it.block(), EDGE_GOTO);
  if (goto_edge != nullptr) {
    auto next_block = goto_edge->target();
    if (next_block->starts_with_move_result()) {
      return cfg::InstructionIterator(
          *this, next_block,
          ir_list::InstructionIterator(next_block->get_first_insn(),
                                       next_block->end()));
    }
  }
  return end;
}

/*
 * fill `new_cfg` with a copy of `this`
 */
void ControlFlowGraph::deep_copy(ControlFlowGraph* new_cfg) const {
  always_assert(editable());
  new_cfg->m_editable = true;
  new_cfg->set_registers_size(this->get_registers_size());

  std::unordered_map<const Edge*, Edge*> old_edge_to_new;
  size_t num_edges = this->m_edges.size();
  new_cfg->m_edges.reserve(num_edges);
  old_edge_to_new.reserve(num_edges);
  for (const Edge* old_edge : this->m_edges) {
    // this shallowly copies block pointers inside, then we patch them later
    Edge* new_edge = new Edge(*old_edge);
    new_cfg->m_edges.insert(new_edge);
    old_edge_to_new.emplace(old_edge, new_edge);
  }

  // copy the code itself
  MethodItemEntryCloner cloner;
  for (const auto& entry : this->m_blocks) {
    const Block* block = entry.second;
    // this shallowly copies edge pointers inside, then we patch them later
    Block* new_block = new Block(*block, &cloner);
    new_block->m_parent = new_cfg;
    new_cfg->m_blocks.emplace(new_block->id(), new_block);
  }
  // We need a second pass because parent position pointers may refer to
  // positions in a block that would be processed later.
  cloner.fix_parent_positions();

  // patch the edge pointers in the blocks to their new cfg counterparts
  for (auto& entry : new_cfg->m_blocks) {
    Block* b = entry.second;
    for (Edge*& e : b->m_preds) {
      e = old_edge_to_new.at(e);
    }
    for (Edge*& e : b->m_succs) {
      e = old_edge_to_new.at(e);
    }
  }

  // patch the block pointers in the edges to their new cfg counterparts
  for (Edge* e : new_cfg->m_edges) {
    e->m_src = new_cfg->m_blocks.at(e->m_src->id());
    e->m_target = new_cfg->m_blocks.at(e->m_target->id());
  }

  // update the entry and exit block pointers to their new cfg counterparts
  new_cfg->m_entry_block = new_cfg->m_blocks.at(this->m_entry_block->id());
  if (this->m_exit_block != nullptr) {
    new_cfg->m_exit_block = new_cfg->m_blocks.at(this->m_exit_block->id());
  }
}

InstructionIterator ControlFlowGraph::find_insn(IRInstruction* needle) {
  auto iterable = InstructionIterable(*this);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (it->insn == needle) {
      return it;
    }
  }
  return iterable.end();
}

std::vector<Block*> ControlFlowGraph::order() {
  // We must simplify first to remove any unreachable blocks
  simplify();

  // This is a modified Weak Topological Ordering (WTO). We create "chains" of
  // blocks that will be kept together, then feed these chains to WTO for it to
  // choose the ordering of the chains. Then, we deconstruct the chains to get
  // an ordering of the blocks.

  // hold the chains of blocks here, though they mostly will be accessed via the
  // map
  std::vector<std::unique_ptr<Chain>> chains;
  // keep track of which blocks are in each chain, for quick lookup.
  std::unordered_map<Block*, Chain*> block_to_chain;
  block_to_chain.reserve(m_blocks.size());

  build_chains(&chains, &block_to_chain);
  const auto& result = wto_chains(block_to_chain);

  always_assert_log(result.size() == m_blocks.size(),
                    "result has %lu blocks, m_blocks has %lu", result.size(),
                    m_blocks.size());
  return result;
}

void ControlFlowGraph::build_chains(
    std::vector<std::unique_ptr<Chain>>* chains,
    std::unordered_map<Block*, Chain*>* block_to_chain) {
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    if (block_to_chain->count(b) != 0) {
      continue;
    }

    always_assert_log(!DEBUG || !b->starts_with_move_result(), "%d is wrong %s",
                      b->id(), SHOW(*this));
    auto unique = std::make_unique<Chain>();
    Chain* chain = unique.get();
    chains->push_back(std::move(unique));

    chain->push_back(b);
    block_to_chain->emplace(b, chain);

    auto goto_edge = get_succ_edge_of_type(b, EDGE_GOTO);
    while (goto_edge != nullptr) {
      // make sure we handle a chain of blocks that all start with move-results
      auto goto_block = goto_edge->target();
      always_assert_log(!DEBUG || m_blocks.count(goto_block->id()) > 0,
                        "bogus block reference %d -> %d in %s",
                        goto_edge->src()->id(), goto_block->id(), SHOW(*this));
      if (goto_block->starts_with_move_result() || goto_block->same_try(b)) {
        // If the goto edge leads to a block with a move-result(-pseudo), then
        // that block must be placed immediately after this one because we can't
        // insert anything between an instruction and its move-result(-pseudo).
        //
        // We also add gotos that are in the same try because we can minimize
        // instructions (by using fallthroughs) without adding another try
        // region. This is not required, but empirical evidence shows that it
        // generates smaller dex files.
        const auto& pair = block_to_chain->emplace(goto_block, chain);
        bool was_already_there = !pair.second;
        if (was_already_there) {
          break;
        }
        chain->push_back(goto_block);
        goto_edge = get_succ_edge_of_type(goto_block, EDGE_GOTO);
      } else {
        break;
      }
    }
  }
}

std::vector<Block*> ControlFlowGraph::wto_chains(
    const std::unordered_map<Block*, Chain*>& block_to_chain) {
  sparta::WeakTopologicalOrdering<Chain*> wto(
      block_to_chain.at(entry_block()), [&block_to_chain](Chain* const& chain) {
        // The chain successor function returns all the outgoing edges' target
        // chains. Where outgoing means that the edge does not go to this chain.
        //
        // FIXME: this algorithm ignores real infinite loops in the block graph
        std::vector<Chain*> result;
        result.reserve(chain->size());

        // TODO: Sort the outputs by edge type, case key, and throw index
        //  * We may be able to use fewer debug positions if we emit case blocks
        //    in the original order.
        //  * Right now, it seems the switches are being output in reverse
        //    order, which is annoying for writing tests.
        const auto& end = chain->end();
        for (auto it = chain->begin(); it != end;) {
          Block* b = *it;
          const auto& next_it = std::next(it);
          Block* next = (next_it == end) ? nullptr : *next_it;

          for (Edge* e : b->succs()) {
            if (e->target() == next) {
              // The most common intra-chain edge is a GOTO to the very next
              // block. Let's cheaply detect this case and filter it early,
              // before we have to do an expensive map lookup.
              continue;
            }
            const auto& succ_chain = block_to_chain.at(e->target());
            // Filter out any edges within this chain. We don't want to
            // erroneously create infinite loops in the chain graph that don't
            // exist in the block graph.
            if (succ_chain != chain) {
              result.push_back(succ_chain);
            }
          }
          it = next_it;
        }
        return result;
      });

  // recursively iterate through the wto order and collect the blocks here
  // This is a depth first traversal. TODO: would breadth first be better?
  std::vector<Block*> wto_order;
  std::function<void(const sparta::WtoComponent<Chain*>&)> get_order;
  get_order = [&get_order, &wto_order](const sparta::WtoComponent<Chain*>& v) {
    for (Block* b : *v.head_node()) {
      wto_order.push_back(b);
    }
    if (v.is_scc()) {
      for (const auto& inner : v) {
        get_order(inner);
      }
    }
  };
  for (const auto& v : wto) {
    get_order(v);
  }
  return wto_order;
}

// Add an MFLOW_TARGET at the end of each edge.
// Insert GOTOs where necessary.
void ControlFlowGraph::insert_branches_and_targets(
    const std::vector<Block*>& ordering) {
  for (auto it = ordering.begin(); it != ordering.end(); ++it) {
    Block* b = *it;

    for (const Edge* edge : b->succs()) {
      if (edge->type() == EDGE_BRANCH) {
        auto branch_it = b->get_conditional_branch();
        always_assert_log(branch_it != b->end(), "block %d %s", b->id(),
                          SHOW(*this));
        auto& branch_mie = *branch_it;

        BranchTarget* bt =
            edge->m_case_key != boost::none
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

// remove all try and catch markers because we may reorder the blocks
void ControlFlowGraph::remove_try_catch_markers() {
  always_assert(m_editable);
  for (const auto& entry : m_blocks) {
    Block* b = entry.second;
    b->m_entries.remove_and_dispose_if([](const MethodItemEntry& mie) {
      return mie.type == MFLOW_TRY || mie.type == MFLOW_CATCH;
    });
  }
}

IRList* ControlFlowGraph::linearize() {
  always_assert(m_editable);
  sanity_check();
  IRList* result = new IRList;

  TRACE(CFG, 5, "before linearize:\n%s", SHOW(*this));

  const std::vector<Block*>& ordering = order();
  insert_branches_and_targets(ordering);
  insert_try_catch_markers(ordering);

  for (Block* b : ordering) {
    result->splice(result->end(), b->m_entries);
  }
  remove_duplicate_positions(result);

  return result;
}

void ControlFlowGraph::insert_try_catch_markers(
    const std::vector<Block*>& ordering) {
  // add back the TRY START, TRY_ENDS, and, MFLOW_CATCHes

  const auto& insert_try_marker_between =
      [this](Block* prev, MethodItemEntry* new_try_marker, Block* b) {
        auto first_it = b->get_first_insn();
        if (first_it != b->end() &&
            opcode::is_move_result_pseudo(first_it->insn->opcode())) {
          // Make sure we don't split up a move-result-pseudo and its primary
          // instruction by placing the marker after the move-result-pseudo
          //
          // TODO: relax the constraint that move-result-pseudo must be
          // immediately after its partner, allowing non-opcode
          // MethodItemEntries between
          b->m_entries.insert_after(first_it, *new_try_marker);
        } else if (new_try_marker->tentry->type == TRY_START) {
          if (prev == nullptr && b == entry_block()) {
            // Parameter loading instructions come before a TRY_START
            auto params = b->m_entries.get_param_instructions();
            b->m_entries.insert_before(params.end(), *new_try_marker);
          } else {
            // TRY_START belongs at the front of a block
            b->m_entries.push_front(*new_try_marker);
          }
        } else {
          // TRY_END belongs at the end of a block
          prev->m_entries.push_back(*new_try_marker);
        }
      };

  std::unordered_map<MethodItemEntry*, Block*> catch_to_containing_block;
  Block* prev = nullptr;
  MethodItemEntry* active_catch = nullptr;
  for (auto it = ordering.begin(); it != ordering.end(); prev = *(it++)) {
    Block* b = *it;
    MethodItemEntry* new_catch = create_catch(b, &catch_to_containing_block);

    if (new_catch == nullptr && cannot_throw(b) && !b->is_catch()) {
      // Generate fewer try regions by merging blocks that cannot throw into the
      // previous try region.
      //
      // But, we have to be careful to not include the catch block of this try
      // region, which would create invalid Dex Try entries. For any given try
      // region, none of its catches may be inside that region.
      continue;
    }

    if (active_catch != new_catch) {
      // If we're switching try regions between these blocks, the TRY_END must
      // come first then the TRY_START. We insert the TRY_START earlier because
      // we're using `insert_after` which inserts things in reverse order
      if (new_catch != nullptr) {
        // Start a new try region before b
        auto new_start = new MethodItemEntry(TRY_START, new_catch);
        insert_try_marker_between(prev, new_start, b);
      }
      if (active_catch != nullptr) {
        // End the current try region before b
        auto new_end = new MethodItemEntry(TRY_END, active_catch);
        insert_try_marker_between(prev, new_end, b);
      }
      active_catch = new_catch;
    }
  }
  if (active_catch != nullptr) {
    always_assert_log(active_catch->centry->next != active_catch,
                      "Invalid cycle: %s", SHOW(active_catch));
    ordering.back()->m_entries.push_back(
        *new MethodItemEntry(TRY_END, active_catch));
  }
}

MethodItemEntry* ControlFlowGraph::create_catch(
    Block* block,
    std::unordered_map<MethodItemEntry*, Block*>* catch_to_containing_block) {
  always_assert(m_editable);

  using EdgeVector = std::vector<Edge*>;
  EdgeVector throws = get_succ_edges_of_type(block, EDGE_THROW);
  if (throws.empty()) {
    // No need to create a catch if there are no throws
    return nullptr;
  }

  std::sort(throws.begin(), throws.end(), [](const Edge* e1, const Edge* e2) {
    return e1->m_throw_info->index < e2->m_throw_info->index;
  });
  const auto& throws_end = throws.end();

  // recurse through `throws` adding catch entries to blocks at the ends of
  // throw edges and connecting the catch entry `next` pointers according to the
  // throw edge indices.
  //
  // We stop early if we find find an equivalent linked list of catch entries
  std::function<MethodItemEntry*(const EdgeVector::iterator&)> add_catch;
  add_catch = [this, &add_catch, &throws_end, catch_to_containing_block](
                  const EdgeVector::iterator& it) -> MethodItemEntry* {
    if (it == throws_end) {
      return nullptr;
    }
    auto edge = *it;
    auto catch_block = edge->target();
    for (auto& mie : *catch_block) {
      // Is there already a catch here that's equivalent to the catch we would
      // create?
      if (mie.type == MFLOW_CATCH &&
          catch_entries_equivalent_to_throw_edges(&mie, it, throws_end,
                                                  *catch_to_containing_block)) {
        // The linked list of catch entries starting at `mie` is equivalent to
        // the rest of `throws` from `it` to `end`. So we don't need to create
        // another one, use the existing list.
        return &mie;
      }
    }
    // We recurse and find the next catch before creating this catch because
    // otherwise, we could create a cycle of the catch entries.
    MethodItemEntry* next = add_catch(std::next(it));

    // create a new catch entry and insert it into the bytecode
    auto new_catch = new MethodItemEntry(edge->m_throw_info->catch_type);
    new_catch->centry->next = next;
    catch_block->m_entries.push_front(*new_catch);
    catch_to_containing_block->emplace(new_catch, catch_block);
    return new_catch;
  };
  return add_catch(throws.begin());
}

// Follow the catch entry linked list starting at `first_mie` and check if the
// throw edges (pointed to by `it`) are equivalent to the linked list. The throw
// edges should be sorted by their indices.
//
// This function is useful in avoiding generating multiple identical catch
// entries
bool ControlFlowGraph::catch_entries_equivalent_to_throw_edges(
    MethodItemEntry* first_mie,
    std::vector<Edge*>::iterator it,
    std::vector<Edge*>::iterator end,
    const std::unordered_map<MethodItemEntry*, Block*>&
        catch_to_containing_block) {

  for (auto mie = first_mie; mie != nullptr; mie = mie->centry->next) {
    always_assert(mie->type == MFLOW_CATCH);
    if (it == end) {
      return false;
    }
    auto edge = *it;

    if (mie->centry->catch_type != edge->m_throw_info->catch_type) {
      return false;
    }

    const auto& search = catch_to_containing_block.find(mie);
    always_assert_log(search != catch_to_containing_block.end(),
                      "%s not found in %s", SHOW(*mie), SHOW(*this));
    if (search->second != edge->target()) {
      return false;
    }

    ++it;
  }
  return it == end;
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

  for (Edge* e : m_edges) {
    delete e;
  }
}

Block* ControlFlowGraph::create_block() {
  size_t id = next_block_id();
  Block* b = new Block(this, id);
  m_blocks.emplace(id, b);
  return b;
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
      t
      visit(const Block* b) {
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

std::vector<Block*> ControlFlowGraph::real_exit_blocks(
    bool include_infinite_loops) {
  std::vector<Block*> result;
  if (m_exit_block != nullptr && include_infinite_loops) {
    auto ghosts = get_pred_edges_of_type(m_exit_block, EDGE_GHOST);
    if (!ghosts.empty()) {
      // The exit block is a ghost block, ignore it and get the real exit
      // points.
      for (auto e : ghosts) {
        result.push_back(e->src());
      }
    } else {
      // Empty ghosts means the method has a single exit point and
      // calculate_exit_block didn't add a ghost block.
      result.push_back(m_exit_block);
    }
  } else {
    always_assert_log(!include_infinite_loops,
                      "call calculate_exit_block first");
    for (const auto& entry : m_blocks) {
      Block* block = entry.second;
      const auto& b = block->branchingness();
      if (b == opcode::BRANCH_RETURN || b == opcode::BRANCH_THROW) {
        result.push_back(block);
      }
    }
  }
  return result;
}

std::vector<Block*> ControlFlowGraph::return_blocks() const {
  std::vector<Block*> result;
  for (const auto& entry : m_blocks) {
    Block* block = entry.second;
    const auto& b = block->branchingness();
    if (b == opcode::BRANCH_RETURN) {
      result.push_back(block);
    }
  }
  return result;
}

/*
 * Find all exit blocks. Note that it's not as simple as looking for Blocks with
 * return or throw opcodes; infinite loops are a valid way of terminating dex
 * bytecode too. As such, we need to find all strongly connected components
 * (SCCs) and vertices that lack successors. For SCCs that lack successors, any
 * one of its vertices can be treated as an exit block; this implementation
 * picks the head of the SCC.
 */
void ControlFlowGraph::calculate_exit_block() {
  if (m_exit_block != nullptr) {
    if (!m_editable) {
      return;
    }
    if (get_pred_edge_of_type(m_exit_block, EDGE_GHOST) != nullptr) {
      // Need to clear old exit block before recomputing the exit of a CFG
      // with multiple exit points
      remove_block(m_exit_block);
      m_exit_block = nullptr;
    }
  }

  ExitBlocks eb;
  eb.visit(entry_block());
  if (eb.exit_blocks.size() == 1) {
    m_exit_block = eb.exit_blocks[0];
  } else {
    m_exit_block = create_block();
    for (Block* b : eb.exit_blocks) {
      add_edge(b, m_exit_block, EDGE_GHOST);
    }
  }
}

// public API edge removal functions
void ControlFlowGraph::delete_edge(Edge* edge) {
  remove_edge(edge);
  free_edge(edge);
}

void ControlFlowGraph::delete_succ_edges(Block* b) {
  free_edges(remove_succ_edges(b));
}

void ControlFlowGraph::delete_pred_edges(Block* b) {
  free_edges(remove_pred_edges(b));
}

// private edge removal functions
//   These are raw removal, they don't free the edge.
ControlFlowGraph::EdgeSet ControlFlowGraph::remove_edges_between(Block* p,
                                                                 Block* s,
                                                                 bool cleanup) {
  return remove_edge_if(p, s, [](const Edge*) { return true; }, cleanup);
}

void ControlFlowGraph::delete_edges_between(Block* p, Block* s) {
  free_edges(remove_edges_between(p, s));
}

void ControlFlowGraph::remove_edge(Edge* edge, bool cleanup) {
  remove_edge_if(edge->src(), edge->target(),
                 [edge](const Edge* e) { return edge == e; }, cleanup);
}

// After `edges` have been removed from the graph,
//   * Turn BRANCHes/SWITCHes with one outgoing edge into GOTOs
void ControlFlowGraph::cleanup_deleted_edges(const EdgeSet& edges) {
  for (Edge* e : edges) {
    auto pred_block = e->src();
    auto last_it = pred_block->get_last_insn();
    if (last_it != pred_block->end()) {
      auto last_insn = last_it->insn;
      auto op = last_insn->opcode();
      auto remaining_forward_edges = pred_block->succs();
      if ((is_conditional_branch(op) || is_switch(op)) &&
          remaining_forward_edges.size() == 1) {
        pred_block->m_entries.erase_and_dispose(last_it);
        remaining_forward_edges.at(0)->m_type = EDGE_GOTO;
      }
    }
  }
}

void ControlFlowGraph::free_edge(Edge* edge) {
  m_edges.erase(edge);
  delete edge;
}

void ControlFlowGraph::free_edges(const EdgeSet& edges) {
  for (Edge* e : edges) {
    free_edge(e);
  }
}

Edge* ControlFlowGraph::get_pred_edge_of_type(const Block* block,
                                              EdgeType type) const {
  return get_pred_edge_if(block,
                          [type](const Edge* e) { return e->type() == type; });
}

Edge* ControlFlowGraph::get_succ_edge_of_type(const Block* block,
                                              EdgeType type) const {
  return get_succ_edge_if(block,
                          [type](const Edge* e) { return e->type() == type; });
}

std::vector<Edge*> ControlFlowGraph::get_pred_edges_of_type(
    const Block* block, EdgeType type) const {
  return get_pred_edges_if(block,
                           [type](const Edge* e) { return e->type() == type; });
}
std::vector<Edge*> ControlFlowGraph::get_succ_edges_of_type(
    const Block* block, EdgeType type) const {
  return get_succ_edges_if(block,
                           [type](const Edge* e) { return e->type() == type; });
}

Block* ControlFlowGraph::split_block(const cfg::InstructionIterator& it) {
  always_assert(editable());
  always_assert(!it.is_end());

  // old_block will be the predecessor
  Block* old_block = it.block();
  // new_block will be the successor
  Block* new_block = create_block();
  const IRList::iterator& raw_it = it.unwrap();
  always_assert(raw_it != old_block->get_last_insn());

  // move the rest of the instructions after the callsite into the new block
  new_block->m_entries.splice_selection(new_block->begin(),
                                        old_block->m_entries, std::next(raw_it),
                                        old_block->end());
  // make the outgoing edges come from the new block
  std::vector<Edge*> to_move(old_block->succs().begin(),
                             old_block->succs().end());
  for (auto e : to_move) {
    set_edge_source(e, new_block);
  }
  // connect the halves of the block we just split up
  add_edge(old_block, new_block, EDGE_GOTO);
  return new_block;
}

void ControlFlowGraph::merge_blocks(Block* pred, Block* succ) {
  const auto& not_throws = [](const Edge* e) {
    return e->type() != EDGE_THROW;
  };
  {
    const auto& forwards = get_succ_edges_if(pred, not_throws);
    always_assert(forwards.size() == 1);
    auto forward_edge = forwards[0];
    always_assert(forward_edge->target() == succ);
    always_assert(forward_edge->type() == EDGE_GOTO);
    const auto& reverses = succ->preds();
    always_assert(reverses.size() == 1);
    auto reverse_edge = reverses[0];
    always_assert(forward_edge == reverse_edge);
  }

  delete_edges_between(pred, succ);
  // move succ's code into pred
  pred->m_entries.splice(pred->m_entries.end(), succ->m_entries);

  // move succ's outgoing edges to pred.
  // Intentionally copy the vector of edges because set_edge_source edits the
  // edge vectors
  auto succs = get_succ_edges_if(succ, not_throws);
  for (auto succ_edge : succs) {
    set_edge_source(succ_edge, pred);
  }

  // remove the succ block
  delete_pred_edges(succ);
  delete_succ_edges(succ);
  m_blocks.erase(succ->id());
  delete succ;
}

void ControlFlowGraph::set_edge_target(Edge* edge, Block* new_target) {
  move_edge(edge, nullptr, new_target);
}

void ControlFlowGraph::set_edge_source(Edge* edge, Block* new_source) {
  move_edge(edge, new_source, nullptr);
}

// Move this edge out of the vectors between its old blocks
// and into the vectors between the new blocks
void ControlFlowGraph::move_edge(Edge* edge,
                                 Block* new_source,
                                 Block* new_target) {
  // remove this edge from the graph temporarily but do not delete it because
  // we're going to move it elsewhere
  remove_edge(edge, /* cleanup */ false);

  if (new_source != nullptr) {
    edge->m_src = new_source;
  }
  if (new_target != nullptr) {
    edge->m_target = new_target;
  }

  edge->src()->m_succs.push_back(edge);
  edge->target()->m_preds.push_back(edge);
}

bool ControlFlowGraph::blocks_are_in_same_try(const Block* b1,
                                              const Block* b2) const {
  const auto& throws1 = b1->get_outgoing_throws_in_order();
  const auto& throws2 = b2->get_outgoing_throws_in_order();
  if (throws1.size() != throws2.size()) {
    return false;
  }
  auto it1 = throws1.begin();
  auto it2 = throws2.begin();
  for (; it1 != throws1.end(); ++it1, ++it2) {
    auto e1 = *it1;
    auto e2 = *it2;
    if (e1->target() != e2->target() ||
        e1->m_throw_info->catch_type != e2->m_throw_info->catch_type) {
      return false;
    }
  }
  return true;
}

void ControlFlowGraph::remove_opcode(const InstructionIterator& it) {
  always_assert(m_editable);

  MethodItemEntry& mie = *it;
  auto insn = mie.insn;
  auto op = insn->opcode();
  always_assert_log(op != OPCODE_GOTO,
                    "There are no GOTO instructions in the CFG");
  Block* block = it.block();
  auto last_it = block->get_last_insn();
  always_assert_log(last_it != block->end(), "cannot remove from empty block");

  if (is_conditional_branch(op) || is_switch(op)) {
    // Remove all outgoing EDGE_BRANCHes
    // leaving behind only an EDGE_GOTO (and maybe an EDGE_THROW?)
    //
    // Don't cleanup because we're deleting the instruction at the end of this
    // function
    free_edges(remove_succ_edge_if(
        block, [](const Edge* e) { return e->type() == EDGE_BRANCH; },
        /* cleanup */ false));
  } else if (insn->has_move_result_pseudo()) {
    // delete the move-result-pseudo too
    if (insn == last_it->insn) {
      // The move-result-pseudo is in the next (runtime) block.
      // We follow the goto edge to the block that should have the
      // move-result-pseudo.
      //
      // We can't use std::next because that goes to the next block in ID order,
      // which may not be the next runtime block.
      auto goto_edge = get_succ_edge_of_type(block, EDGE_GOTO);
      auto move_result_block = goto_edge->target();
      auto first_it = move_result_block->get_first_insn();
      always_assert(first_it != move_result_block->end());
      always_assert_log(opcode::is_move_result_pseudo(first_it->insn->opcode()),
                        "%d -> %d in %s", block->id(), move_result_block->id(),
                        SHOW(*this));
      // We can safely delete this move-result-pseudo because it cannot be the
      // move-result-pseudo of more than one primary instruction. A CFG with
      // multiple edges to a block beginning with a move-result-pseudo is a
      // malformed CFG.
      always_assert_log(move_result_block->preds().size() == 1,
                        "Multiple edges to a move-result-pseudo in %d. %s",
                        move_result_block->id(), SHOW(*this));
      move_result_block->m_entries.erase_and_dispose(first_it);
    } else {
      // The move-result-pseudo is in the same block as this one.
      // This occurs when we're not in a try region.
      auto mrp_it = std::next(it);
      always_assert(mrp_it.block() == block);
      block->m_entries.erase_and_dispose(mrp_it.unwrap());
    }
  }

  if (insn == last_it->insn && (opcode::may_throw(op) || op == OPCODE_THROW)) {
    // We're deleting the last instruction that may throw, this block no longer
    // throws. We should remove the throw edges
    delete_succ_edge_if(block,
                        [](const Edge* e) { return e->type() == EDGE_THROW; });
  }

  // delete the requested instruction
  block->m_entries.erase_and_dispose(it.unwrap());
}

void ControlFlowGraph::create_branch(Block* b,
                                     IRInstruction* insn,
                                     Block* fls,
                                     Block* tru) {
  create_branch(b, insn, fls, {{1, tru}});
}

void ControlFlowGraph::create_branch(
    Block* b,
    IRInstruction* insn,
    Block* goto_block,
    const std::vector<std::pair<int32_t, Block*>>& case_to_block) {
  auto op = insn->opcode();
  always_assert(m_editable);
  always_assert_log(is_branch(op), "%s is not a branch instruction", SHOW(op));
  always_assert_log(!is_goto(op),
                    "There are no gotos in the editable CFG. Use add_edge()");

  auto existing_last = b->get_last_insn();
  if (existing_last != b->end()) {
    auto last_op = existing_last->insn->opcode();
    always_assert_log(
        !(is_branch(last_op) || is_throw(last_op) || is_return(last_op)),
        "Can't add branch after %s in Block %d in %s",
        SHOW(existing_last->insn), b->id(), SHOW(*this));
  }

  auto existing_goto_edge = get_succ_edge_of_type(b, EDGE_GOTO);
  if (goto_block != nullptr) {
    if (existing_goto_edge != nullptr) {
      // redirect it
      set_edge_target(existing_goto_edge, goto_block);
    } else {
      add_edge(b, goto_block, EDGE_GOTO);
    }
  } else {
    always_assert_log(existing_goto_edge != nullptr,
                      "%s must have a false case", SHOW(insn));
  }

  b->m_entries.push_back(*new MethodItemEntry(insn));
  if (is_switch(op)) {
    for (const auto& entry : case_to_block) {
      add_edge(b, entry.second, entry.first);
    }
  } else {
    always_assert(is_conditional_branch(op));
    always_assert_log(case_to_block.size() == 1,
                      "Wrong number of non-goto cases (%d) for %s",
                      case_to_block.size(), SHOW(op));
    const auto& entry = case_to_block[0];
    always_assert_log(entry.first == 1, "%s only has boolean case key values",
                      SHOW(op));
    add_edge(b, entry.second, EDGE_BRANCH);
  }
}

bool ControlFlowGraph::insert(const InstructionIterator& position,
                              const std::vector<IRInstruction*>& insns,
                              bool before) {
  // Convert to the before case by moving the position forward one.
  Block* b = position.block();
  if (position.unwrap() == b->end()) {
    always_assert_log(before, "can't insert after the end");
  }
  IRList::iterator pos =
      before ? position.unwrap() : std::next(position.unwrap());

  bool invalidated_its = false;
  for (auto insn : insns) {
    const auto& throws = get_succ_edges_of_type(b, EDGE_THROW);

    // Certain types of blocks cannot have instructions added to the end.
    // Disallow that case here.
    if (pos == b->end()) {
      auto existing_last = b->get_last_insn();
      if (existing_last != b->end()) {
        // TODO? Currently, this will abort if someone tries to insert after a
        // may_throw that has a catch in this method. Maybe instead we should
        // insert after the move-result in the goto successor block. That's
        // probably what the user meant to do anyway.
        auto last_op = existing_last->insn->opcode();
        always_assert_log(!is_branch(last_op) && !is_throw(last_op) &&
                              !is_return(last_op) && throws.empty(),
                          "Can't add instructions after %s in Block %d in %s",
                          SHOW(existing_last->insn), b->id(), SHOW(*this));
      }
    }

    auto op = insn->opcode();
    always_assert_log(!is_branch(op),
                      "insert() does not support branch opcodes. Use "
                      "create_branch() instead");

    IRList::iterator new_inserted_it = b->m_entries.insert_before(pos, insn);

    if (is_throw(op) || is_return(op)) {
      // Throw and return end the block, we must remove all code after them.
      always_assert(insn == insns.back());
      for (auto it = pos; it != b->m_entries.end();) {
        it = b->m_entries.erase_and_dispose(it);
        invalidated_its = true;
      }
      if (is_return(op)) {
        // This block now ends in a return, it must have no successors.
        delete_succ_edges(b);
      } else {
        always_assert(is_throw(op));
        // The only valid way to leave this block is via a throw edge.
        delete_succ_edge_if(b, [](const Edge* e) {
          return !(e->type() == EDGE_THROW || e->type() == EDGE_GHOST);
        });
      }
      // If this created unreachable blocks, they will be removed by simplify.
    } else if (opcode::may_throw(op)) {
      if (!throws.empty()) {
        invalidated_its = true;
        // FIXME: Copying the outgoing throw edges isn't enough.
        // When the editable CFG is constructed, we transform the try regions
        // into throw edges. We only add these edges to blocks that may throw,
        // thus losing the knowledge of which blocks were originally inside a
        // try region. If we add a new throwing instruction here. It may be
        // added to a block that was originally inside a try region, but we lost
        // that information already.
        //
        // Possible Solutions:
        // * Rework throw representation to regions instead of duplicated edges?
        // * User gives a block that we want to copy the throw edges from?
        // * User specifies which throw edges they want and to which blocks?

        // Split the block after the new instruction.
        // b has become the predecessor of the new split pair
        Block* succ =
            split_block(b->to_cfg_instruction_iterator(new_inserted_it));

        // Copy the outgoing throw edges of the original block into the block
        // that now ends with the new instruction
        for (const Edge* e : throws) {
          Edge* copy = new Edge(*e);
          copy->m_src = b;
          add_edge(copy);
        }
        // Continue inserting in the successor block.
        b = succ;
        pos = succ->begin();
      }
    }
  }
  return invalidated_its;
}

bool ControlFlowGraph::insert_before(const InstructionIterator& position,
                                     const std::vector<IRInstruction*>& insns) {
  return insert(position, insns, /* before */ true);
}

bool ControlFlowGraph::insert_after(const InstructionIterator& position,
                                    const std::vector<IRInstruction*>& insns) {
  return insert(position, insns, /* before */ false);
}

bool ControlFlowGraph::push_front(Block* b,
                                  const std::vector<IRInstruction*>& insns) {
  const auto& begin = ir_list::InstructionIterable(b).begin();
  return insert(b->to_cfg_instruction_iterator(begin), insns,
                /* before */ true);
}

bool ControlFlowGraph::push_back(Block* b,
                                 const std::vector<IRInstruction*>& insns) {
  const auto& end = ir_list::InstructionIterable(b).end();
  return insert(b->to_cfg_instruction_iterator(end), insns, /* before */ true);
}

bool ControlFlowGraph::insert_before(const InstructionIterator& position,
                                     IRInstruction* insn) {
  return insert_before(position, std::vector<IRInstruction*>{insn});
}

bool ControlFlowGraph::insert_after(const InstructionIterator& position,
                                    IRInstruction* insn) {
  return insert_after(position, std::vector<IRInstruction*>{insn});
}

bool ControlFlowGraph::push_front(Block* b, IRInstruction* insn) {
  return push_front(b, std::vector<IRInstruction*>{insn});
}

bool ControlFlowGraph::push_back(Block* b, IRInstruction* insn) {
  return push_back(b, std::vector<IRInstruction*>{insn});
}

void ControlFlowGraph::remove_block(Block* block) {
  if (block == entry_block()) {
    always_assert(block->succs().size() == 1);
    set_entry_block(block->succs()[0]->target());
  }
  delete_pred_edges(block);
  delete_succ_edges(block);

  std::unordered_set<DexPosition*> deleted_positions;
  for (const auto& mie : *block) {
    if (mie.type == MFLOW_POSITION) {
      deleted_positions.insert(mie.pos.get());
    }
  }
  remove_dangling_parents(deleted_positions);

  auto id = block->id();
  auto num_removed = m_blocks.erase(id);
  always_assert_log(num_removed == 1,
                    "Block %d wasn't in CFG. Attempted double delete?", id);
  block->m_entries.clear_and_dispose();
  delete block;
}

// delete old_block and reroute its predecessors to new_block
void ControlFlowGraph::replace_block(Block* old_block, Block* new_block) {
  std::vector<Edge*> to_redirect = old_block->preds();
  for (auto e : to_redirect) {
    set_edge_target(e, new_block);
  }
  remove_block(old_block);
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

std::vector<Block*> postorder_sort(const std::vector<Block*>& cfg) {
  std::vector<Block*> postorder;
  std::vector<Block*> stack;
  std::unordered_set<Block*> visited;
  for (size_t i = 1; i < cfg.size(); i++) {
    if (cfg[i]->preds().empty()) {
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
    if (block->preds().empty()) {
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
      if (ordered_block->preds().empty()) {
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

ControlFlowGraph::EdgeSet ControlFlowGraph::remove_succ_edges(Block* b,
                                                              bool cleanup) {
  return remove_succ_edge_if(b, [](const Edge*) { return true; }, cleanup);
}

ControlFlowGraph::EdgeSet ControlFlowGraph::remove_pred_edges(Block* b,
                                                              bool cleanup) {
  return remove_pred_edge_if(b, [](const Edge*) { return true; }, cleanup);
}

} // namespace cfg
