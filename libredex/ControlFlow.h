/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <utility>

#include "Transform.h"

enum EdgeType {
  EDGE_GOTO,
  EDGE_BRANCH,
  EDGE_THROW,
  EDGE_TYPE_SIZE
};

struct Block {
  explicit Block(size_t id) : m_id(id) {}

  size_t id() const { return m_id; }
  const std::vector<Block*>& preds() const { return m_preds; }
  const std::vector<Block*>& succs() const { return m_succs; }
  FatMethod::iterator begin() { return m_begin; }
  FatMethod::iterator end() { return m_end; }
  FatMethod::reverse_iterator rbegin() {
    return FatMethod::reverse_iterator(m_end);
  }
  FatMethod::reverse_iterator rend() {
    return FatMethod::reverse_iterator(m_begin);
  }

 private:
  friend class ControlFlowGraph;
  friend class IRCode;

  size_t m_id;
  FatMethod::iterator m_begin;
  FatMethod::iterator m_end;
  std::vector<Block*> m_preds;
  std::vector<Block*> m_succs;
};

inline bool is_catch(Block* b) {
  auto it = b->begin();
  return it->type == MFLOW_CATCH;
}

bool ends_with_may_throw(Block* b, bool end_block_before_throw = true);

/*
 * Build a postorder sorted vector of blocks from the given CFG.  Uses a
 * standard depth-first search with a side table of already-visited nodes.
 */
std::vector<Block*> postorder_sort(const std::vector<Block*>& cfg);


class ControlFlowGraph {
  using IdPair = std::pair<size_t, size_t>;

 public:
  using EdgeFlags = std::bitset<EDGE_TYPE_SIZE>;

  ~ControlFlowGraph();

  const std::vector<Block*>& blocks() const { return m_blocks; }
  Block* create_block();

  const EdgeFlags& edge(Block* pred, Block* succ) const {
    return m_edges.at(IdPair(pred->id(), succ->id()));
  }
  void add_edge(Block* pred, Block* succ, EdgeType type);
  void remove_edge(Block* pred, Block* succ, EdgeType type);
  void remove_all_edges(Block* pred, Block* succ);

 private:
  EdgeFlags& edge(Block* pred, Block* succ) {
    return m_edges[IdPair(pred->id(), succ->id())];
  }

  std::vector<Block*> m_blocks;
  std::unordered_map<IdPair, EdgeFlags, boost::hash<IdPair>> m_edges;
};
