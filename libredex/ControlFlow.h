/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <utility>

#include "FixpointIterators.h"
#include "IRCode.h"

/**
 * A Control Flow Graph is a directed graph of Basic Blocks.
 *
 * Each `Block` has some number of successors and predecessors. `Block`s are
 * connected to their predecessors and successors by `EdgeType`s that specify
 * the type of connection.
 *
 * Right now there are two types of CFGs. Editable and non-editable:
 * A non editable CFG's blocks have begin and end pointers into the big linear
 * IRList in IRCode.
 * An editable CFG's blocks each own a small IRList (with MethodItemEntries
 * taken from IRCode)
 *
 * TODO: Add useful CFG editing methods
 * TODO: phase out edits to the IRCode and move them all to the editable CFG
 * TODO: remove non-editable CFG option
 */

enum EdgeType { EDGE_GOTO, EDGE_BRANCH, EDGE_THROW, EDGE_TYPE_SIZE };

class Block;

// Forward declare friend function of Block to handle cyclic dependency
namespace transform {
void replace_block(IRCode*, Block*, Block*);
}

namespace cfg {

class Edge final {
  Block* m_src;
  Block* m_target;
  EdgeType m_type;

 public:
  Edge(Block* src, Block* target, EdgeType type)
      : m_src(src), m_target(target), m_type(type) {}
  bool operator==(const Edge& that) const {
    return m_src == that.m_src && m_target == that.m_target &&
           m_type == that.m_type;
  }
  Block* src() const { return m_src; }
  Block* target() const { return m_target; }
  EdgeType type() const { return m_type; }
};

} // namespace cfg

// TODO: Put the rest of this header under the cfg namespace too

// A piece of "straight-line" code. Targets are only at the beginning of a block
// and branches (throws, gotos, switches, etc) are only at the end of a block.
class Block {
 public:
  explicit Block(const ControlFlowGraph* parent, size_t id)
      : m_id(id), m_parent(parent) {}

  size_t id() const { return m_id; }
  const std::vector<std::shared_ptr<cfg::Edge>>& preds() const {
    return m_preds;
  }
  const std::vector<std::shared_ptr<cfg::Edge>>& succs() const {
    return m_succs;
  }
  IRList::iterator begin();
  IRList::iterator end();
  IRList::reverse_iterator rbegin() {
    return IRList::reverse_iterator(end());
  }
  IRList::reverse_iterator rend() {
    return IRList::reverse_iterator(begin());
  }

  bool is_catch() {
    return begin()->type == MFLOW_CATCH;
  }

  // remove all debug source code line numbers from this block
  void remove_debug_line_info();

 private:
  friend class ControlFlowGraph;
  friend void transform::replace_block(IRCode*, Block*, Block*);

  // return an iterator to the goto in this block (if there is one)
  // otherwise, return m_entries.end()
  IRList::iterator get_goto();

  // returns a vector of all MFLOW_TARGETs in this block
  std::vector<IRList::iterator> get_targets();

  size_t m_id;

  // MethodItemEntries get moved from IRCode into here (if m_editable)
  // otherwise, this is empty.
  IRList m_entries;

  // TODO delete these
  // These refer into the IRCode IRList
  // These are only used in non-editable mode.
  IRList::iterator m_begin;
  IRList::iterator m_end;

  std::vector<std::shared_ptr<cfg::Edge>> m_preds;
  std::vector<std::shared_ptr<cfg::Edge>> m_succs;

  // This is the successor taken in the
  // non-exception, if false, or switch default situations
  Block* m_default_successor = nullptr;

  // nullptr if not in try region
  MethodItemEntry* m_catch_start = nullptr;

  // the graph that this block belongs to
  const ControlFlowGraph* m_parent = nullptr;
};

struct DominatorInfo {
  Block* dom;
  size_t postorder;
};

class ControlFlowGraph {

 public:
  ControlFlowGraph() = default;
  ControlFlowGraph(const ControlFlowGraph&) = delete;

  /*
   * if editable is false, changes to the CFG aren't reflected in the output dex
   * instructions.
   */
  ControlFlowGraph(IRList* ir,
                   bool editable = false);
  ~ControlFlowGraph();

  /*
   * convert from the graph representation to a list of MethodItemEntries
   */
  IRList* linearize();

  std::vector<Block*> blocks() const;

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

  void add_edge(Block* pred, Block* succ, EdgeType type);

  /*
   * Print the graph in the DOT graph description language.
   */
  std::ostream& write_dot_format(std::ostream&) const;

  Block* find_block_that_ends_here(const IRList::iterator& loc) const;

  // Find a common dominator block that is closest to both block.
  Block* idom_intersect(
      const std::unordered_map<Block*, DominatorInfo>& postorder_dominator,
      Block* block1,
      Block* block2) const;

  // Finding immediate dominator for each blocks in ControlFlowGraph.
  std::unordered_map<Block*, DominatorInfo> immediate_dominators() const;

  void remove_succ_edges(Block* b);

  // Do writes to this CFG propagate back to IR and Dex code?
  bool editable() const { return m_editable; }

 private:
  using BranchToTargets =
      std::unordered_map<MethodItemEntry*, std::vector<Block*>>;
  using TryEnds = std::vector<std::pair<TryEntry*, Block*>>;
  using TryCatches = std::unordered_map<CatchEntry*, Block*>;
  using Boundaries =
      std::unordered_map<Block*,
                         std::pair<IRList::iterator, IRList::iterator>>;
  using Blocks = std::map<size_t, Block*>;

  // Find block boundaries in IRCode and create the blocks
  // For use by the constructor. You probably don't want to call this from
  // elsewhere
  void find_block_boundaries(IRList* ir,
                             BranchToTargets& branch_to_targets,
                             TryEnds& try_ends,
                             TryCatches& try_catches,
                             Boundaries& boundaries);

  // Add edges between blocks created by `find_block_boundaries`
  // For use by the constructor. You probably don't want to call this from
  // elsewhere
  void connect_blocks(BranchToTargets& branch_to_targets);

  // Add edges from try blocks to their catch handlers.
  // For use by the constructor. You probably don't want to call this from
  // elsewhere
  void add_catch_edges(TryEnds& try_ends, TryCatches& try_catches);

  // For use by the constructor. You probably don't want to call this from
  // elsewhere
  void remove_unreachable_succ_edges();

  // Move MethodItemEntries from ir into their blocks
  // For use by the constructor. You probably don't want to call this from
  // elsewhere
  void fill_blocks(IRList* ir, Boundaries& boundaries);

  // add an explicit goto to the implicit fallthroughs so that we can
  // re-arrange blocks safely.
  // For use by the constructor. You probably don't want to call this from
  // elsewhere
  void add_fallthrough_gotos();

  // SIGABORT if the internal state of the CFG is invalid
  void sanity_check();

  // remove all TRY START and ENDs because we may reorder the blocks
  // Assumes m_editable is true
  void remove_try_markers();

  // Targets point to branches. If a branch is deleted then we need to clean up
  // the targets that pointed to that branch
  // TODO: This isn't in use yet. Either use it or delete it.
  void clean_dangling_targets();

  // choose an order of blocks for output
  std::vector<Block*> order();

  void remove_fallthrough_gotos(const std::vector<Block*> ordering);

  void remove_all_edges(Block* pred, Block* succ);

  Blocks m_blocks;
  Block* m_entry_block{nullptr};
  Block* m_exit_block{nullptr};
  bool m_editable;
};

namespace cfg {

// A static-method-only API for use with the monotonic fixpoint iterator.
class GraphInterface : public FixpointIteratorGraphSpec<GraphInterface> {
  ~GraphInterface() = delete;

 public:
  using Graph = ControlFlowGraph;
  using NodeId = Block*;
  using EdgeId = std::shared_ptr<cfg::Edge>;
  static NodeId entry(const Graph& graph) {
    return const_cast<NodeId>(graph.entry_block());
  }
  static NodeId exit(const Graph& graph) {
    return const_cast<NodeId>(graph.exit_block());
  }
  static std::vector<EdgeId> predecessors(const Graph&, const NodeId& b) {
    return b->preds();
  }
  static std::vector<EdgeId> successors(const Graph&, const NodeId& b) {
    return b->succs();
  }
  static NodeId source(const Graph&, const EdgeId& e) { return e->src(); }
  static NodeId target(const Graph&, const EdgeId& e) { return e->target(); }
};

} // namespace cfg

std::vector<Block*> find_exit_blocks(const ControlFlowGraph&);

/*
 * Build a postorder sorted vector of blocks from the given CFG. Uses a
 * standard depth-first search with a side table of already-visited nodes.
 */
std::vector<Block*> postorder_sort(const std::vector<Block*>& cfg);
