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

#include "IRCode.h"

enum EdgeType {
  EDGE_GOTO,
  EDGE_BRANCH,
  EDGE_THROW,
  EDGE_TYPE_SIZE
};

// Forward declare friend function of Block to handle cyclic dependency
namespace transform {
  void replace_block(IRCode*, Block*, Block*);
}

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
  friend void transform::replace_block(IRCode*, Block*, Block*);

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

struct DominatorInfo {
  Block* dom;
  size_t postorder;
};

class ControlFlowGraph {
  using IdPair = std::pair<size_t, size_t>;

 public:
  using EdgeFlags = std::bitset<EDGE_TYPE_SIZE>;

  ~ControlFlowGraph();

  const std::vector<Block*>& blocks() const { return m_blocks; }
  Block* create_block();
  const Block* entry_block() const { return m_entry_block; }
  const Block* exit_block() const { return m_exit_block; }
  Block* entry_block() { return m_entry_block; }
  Block* exit_block() { return m_exit_block; }
  void set_entry_block(Block* b) { m_entry_block = b; }
  void set_exit_block(Block* b) { m_exit_block = b; }
  /*
   * Determine where the exit block is. If there is more than one, create a
   * "ghost" block that is the successor to all of them.
   */
  void calculate_exit_block();

  const EdgeFlags& edge(Block* pred, Block* succ) const {
    return m_edges.at(IdPair(pred->id(), succ->id()));
  }
  void add_edge(Block* pred, Block* succ, EdgeType type);
  void remove_edge(Block* pred, Block* succ, EdgeType type);
  void remove_all_edges(Block* pred, Block* succ);

  /*
   * Print the graph in the DOT graph description language.
   */
  std::ostream& write_dot_format(std::ostream&) const;

  Block* find_block_that_ends_here(const FatMethod::iterator& loc) const;

  // Find a common dominator block that is closest to both block.
  Block* idom_intersect(
      const std::unordered_map<Block*, DominatorInfo>& postorder_dominator,
      Block* block1,
      Block* block2) const;

  // Finding immediate dominator for each blocks in ControlFlowGraph.
  std::unordered_map<Block*, DominatorInfo> immediate_dominators() const;

 private:
  EdgeFlags& mutable_edge(Block* pred, Block* succ) {
    return m_edges[IdPair(pred->id(), succ->id())];
  }

  std::vector<Block*> m_blocks;
  std::unordered_map<IdPair, EdgeFlags, boost::hash<IdPair>> m_edges;
  Block* m_entry_block {nullptr};
  Block* m_exit_block {nullptr};
};

std::vector<Block*> find_exit_blocks(const ControlFlowGraph&);

bool ends_with_may_throw(Block* b, bool end_block_before_throw = true);

/*
 * Build a postorder sorted vector of blocks from the given CFG. Uses a
 * standard depth-first search with a side table of already-visited nodes.
 */
std::vector<Block*> postorder_sort(const std::vector<Block*>& cfg);
