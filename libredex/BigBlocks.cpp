/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BigBlocks.h"

namespace {

static bool is_big_block_successor(cfg::Block* block,
                                   bool ignore_throws = false) {
  // A big block successor is a block that...
  // 1. has only a single GOTO predecessor which has no outgoing BRANCH
  auto& pred_edges = block->preds();
  if (pred_edges.size() != 1 || block == block->cfg().entry_block()) {
    return false;
  }
  auto pred_edge = pred_edges.front();
  if (pred_edge->type() != cfg::EDGE_GOTO) {
    return false;
  }
  auto pred_block = pred_edge->src();
  if (block->cfg().get_succ_edge_of_type(pred_block, cfg::EDGE_BRANCH)) {
    return false;
  }

  // 2. Shares the same try(ies) with its predecessor, or
  //    cannot throw (as may happen in particular in a block that ends with a
  //    conditional branch or return).
  return ignore_throws || pred_block->same_try(block) || block->cannot_throw();
}

} // namespace

namespace big_blocks {

void Iterator::adjust_block() {
  while (m_block && m_it == m_block->end()) {
    m_block = m_block->goes_to();
    if (m_block && is_big_block_successor(m_block, m_ignore_throws)) {
      m_it = m_block->begin();
    } else {
      m_block = nullptr;
    }
  }
}

Iterator::Iterator(cfg::Block* block,
                   const IRList::iterator& it,
                   bool ignore_throws)
    : m_block(block), m_it(it), m_ignore_throws(ignore_throws) {
  adjust_block();
}

Iterator Iterator::operator++(int) {
  Iterator ret(*this);
  ++*this;
  return ret;
}

Iterator& Iterator::operator++() {
  always_assert(m_block != nullptr);
  always_assert(m_it != m_block->end());
  m_it++;
  adjust_block();
  return *this;
}

InstructionIterator InstructionIterator::operator++(int) {
  InstructionIterator ret(*this);
  ++*this;
  return ret;
}
InstructionIterator& InstructionIterator::operator++() {
  auto block = m_it++.block();
  if (!m_it.is_end() && m_it.block() == block) {
    return *this;
  }
  while (true) {
    auto next_block = block->goes_to();
    if (!next_block || !is_big_block_successor(next_block, m_ignore_throws)) {
      auto ii = ir_list::InstructionIterable(block);
      m_it = block->to_cfg_instruction_iterator(ii.end());
      return *this;
    }
    auto next_ii = ir_list::InstructionIterable(next_block);
    auto next_begin = next_ii.begin();
    if (next_begin != next_ii.end()) {
      m_it = next_block->to_cfg_instruction_iterator(next_begin);
      return *this;
    }
    block = next_block;
  }
}

InstructionIterator InstructionIterable::begin() const {
  for (auto block : m_big_block.get_blocks()) {
    auto ii = ir_list::InstructionIterable(block);
    auto begin = ii.begin();
    if (begin != ii.end()) {
      return InstructionIterator(block->to_cfg_instruction_iterator(begin));
    }
  }
  return end();
}

InstructionIterator InstructionIterable::end() const {
  auto& last_block = m_big_block.get_blocks().back();
  return InstructionIterator(last_block->to_cfg_instruction_iterator(
      ir_list::InstructionIterable(last_block).end()));
}

boost::optional<BigBlock> get_big_block(cfg::Block* block) {
  if (is_big_block_successor(block)) {
    return boost::none;
  }
  std::vector<cfg::Block*> blocks;
  do {
    blocks.push_back(block);
    block = block->goes_to();
  } while (block && is_big_block_successor(block));
  return BigBlock(std::move(blocks));
}

std::vector<BigBlock> get_big_blocks(cfg::ControlFlowGraph& cfg) {
  std::vector<BigBlock> res;
  for (auto block : cfg.blocks()) {
    auto big_block = get_big_block(block);
    if (big_block) {
      res.push_back(*big_block);
    }
  }
  return res;
}

} // namespace big_blocks
