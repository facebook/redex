/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

/*
 * "Big blocks" are a sequence of blocks in a cfg that could be one block, were
 * it not for the fact that the blocks are wrapped around by (the same) try
 * blocks, and some instructions can indeed throw.
 */

#include "ControlFlow.h"
#include "IRCode.h"

namespace big_blocks {

class Iterator {
 private:
  cfg::Block* m_block{nullptr};
  IRList::iterator m_it;
  void adjust_block();

 public:
  using value_type = IRList::iterator::value_type;
  using difference_type = IRList::iterator::difference_type;
  using pointer = IRList::iterator::pointer;
  using reference = IRList::iterator::reference;
  using iterator_category = std::input_iterator_tag;

  explicit Iterator(cfg::Block* block, const IRList::iterator& it);
  const IRList::iterator& unwrap() const { return m_it; }
  reference operator*() const { return *m_it; }
  pointer operator->() const { return &(this->operator*()); }
  bool operator==(const Iterator& other) const {
    return m_block == other.m_block && (!m_block || m_it == other.m_it);
  }
  bool operator!=(Iterator& other) const { return !(*this == other); }
  Iterator operator++(int);
  Iterator& operator++();
};

class InstructionIterator {
 private:
  cfg::InstructionIterator m_it;

 public:
  using value_type = cfg::InstructionIterator::value_type;
  using difference_type = cfg::InstructionIterator::difference_type;
  using pointer = cfg::InstructionIterator::pointer;
  using reference = cfg::InstructionIterator::reference;
  using iterator_category = std::input_iterator_tag;

  explicit InstructionIterator(const cfg::InstructionIterator& it) : m_it(it) {}
  const cfg::InstructionIterator& unwrap() const { return m_it; }
  reference operator*() const { return *m_it; }
  pointer operator->() const { return &(this->operator*()); }
  bool operator==(const InstructionIterator& other) const {
    return m_it == other.m_it;
  }
  bool operator!=(const InstructionIterator& other) const {
    return !(*this == other);
  }
  InstructionIterator operator++(int);
  InstructionIterator& operator++();
  cfg::Block* block() const { return m_it.block(); }
};

struct BigBlock {
 private:
  std::vector<cfg::Block*> m_blocks;

 public:
  explicit BigBlock(const std::vector<cfg::Block*>& blocks)
      : m_blocks(blocks) {}

  const std::vector<cfg::Block*>& get_blocks() const { return m_blocks; }
};

struct InstructionIterable {
 private:
  const BigBlock& m_big_block;

 public:
  using iterator = InstructionIterator;
  explicit InstructionIterable(const BigBlock& big_block)
      : m_big_block(big_block) {}
  InstructionIterator begin() const;
  InstructionIterator end() const;
};

// Each block of the original cfg appears in exactly one big block.
std::vector<BigBlock> get_big_blocks(cfg::ControlFlowGraph& cfg);

} // namespace big_blocks
