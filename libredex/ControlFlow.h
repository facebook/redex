/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/dynamic_bitset.hpp>
#include <boost/optional/optional.hpp>
#include <boost/range/sub_range.hpp>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "IRCode.h"

/**
 * A Control Flow Graph is a directed graph of Basic Blocks.
 *
 * Each `Block` has some number of successors and predecessors. `Block`s are
 * connected to their predecessors and successors by `Edge`s that specify
 * the type of connection.
 *
 * EDITABLE MODE:
 * Right now there are two types of CFGs. Editable and non-editable:
 * A non editable CFG's blocks have begin and end pointers into the big linear
 * IRList inside IRCode.
 * An editable CFG's blocks each own a small IRList (with MethodItemEntries
 * taken from IRCode)
 *
 * Editable mode is the new version of the CFG. In the future, it will replace
 * IRCode entirely as the primary code representation. To build an editable CFG,
 * call
 *
 * `code->build_cfg(true)`
 *
 * The editable CFG takes the MethodItemEntries from the IRCode object and moves
 * them into the blocks. The editable CFG steals the code out of the IRCode
 * object. After you've built the CFG in editable mode, you should use the CFG,
 * not the IRCode. The IRCode is empty while the editable CFG exists.
 *
 * You can make all sorts of changes to the CFG and when
 * you're done, move it all back into an IRCode object with
 *
 * `code->clear_cfg()`
 *
 * It is easier to maintain the data structure when there is no unnecessary
 * information duplication. Therefore, MFLOW_TARGETs, OPCODE_GOTOs, MFLOW_TRYs,
 * and MFLOW_CATCHes are deleted and their information is moved to the edges of
 * the CFG.
 *
 * TODO: Add useful CFG editing methods
 * TODO: phase out edits to the IRCode and move them all to the editable CFG
 * TODO: remove non-editable CFG option
 *
 * TODO?: make MethodItemEntry's fields private?
 */

namespace cfg {

enum EdgeType {
  // The false branch of an if statement, default of a switch, or unconditional
  // goto
  EDGE_GOTO,
  // The true branch of an if statement or non-default case of a switch
  EDGE_BRANCH,
  // The edges to a catch block
  EDGE_THROW,
  // A "fake" edge so that we can have a single exit block
  EDGE_GHOST,
  EDGE_TYPE_SIZE
};

class Block;
class ControlFlowGraph;
class CFGInliner;

struct ThrowInfo {
  // nullptr means catch all
  DexType* catch_type;
  // Index from the linked list of `CatchEntry`s in the IRCode. An index of 0
  // denotes the "primary" catch. At runtime, the primary catch is the first
  // block to be checked for equality with the runtime exception type, then onto
  // index 1, etc.
  uint32_t index;

  ThrowInfo(DexType* catch_type, uint32_t index)
      : catch_type(catch_type), index(index) {}

  bool operator==(const ThrowInfo& other) {
    return catch_type == other.catch_type && index == other.index;
  }
};

class Edge final {
 public:
  using CaseKey = int32_t;

 private:
  Block* m_src;
  Block* m_target;
  EdgeType m_type;

  // If this branch is a non-default case of a switch statement, this is the
  // index of the corresponding case block.
  boost::optional<CaseKey> m_case_key;

  std::unique_ptr<ThrowInfo> m_throw_info{nullptr};

  friend class Block;
  friend class ControlFlowGraph;
  friend class CFGInliner;

 public:
  Edge(Block* src, Block* target, EdgeType type)
      : m_src(src), m_target(target), m_type(type) {
    always_assert_log(m_type != EDGE_THROW,
                      "Need a catch type and index to create a THROW edge");
  }
  Edge(Block* src, Block* target, CaseKey case_key)
      : m_src(src),
        m_target(target),
        m_type(EDGE_BRANCH),
        m_case_key(case_key) {}
  Edge(Block* src, Block* target, DexType* catch_type, uint32_t index)
      : m_src(src),
        m_target(target),
        m_type(EDGE_THROW),
        m_throw_info(std::make_unique<ThrowInfo>(catch_type, index)) {}

  /*
   * Copy constructor.
   * Notice that this shallowly copies the block pointers!
   */
  Edge(const Edge& e)
      : m_src(e.m_src),
        m_target(e.m_target),
        m_type(e.m_type),
        m_case_key(e.m_case_key) {
    if (e.m_throw_info) {
      m_throw_info = std::make_unique<ThrowInfo>(*e.m_throw_info);
    }
  }

  bool operator==(const Edge& that) const {
    return m_src == that.m_src && m_target == that.m_target &&
           equals_ignore_source_and_target(that);
  }

  bool operator!=(const Edge& that) const { return !(*this == that); }

  bool equals_ignore_source(const Edge& that) const {
    return m_target == that.m_target && equals_ignore_source_and_target(that);
  }

  bool equals_ignore_target(const Edge& that) const {
    return m_src == that.m_src && equals_ignore_source_and_target(that);
  }

  bool equals_ignore_source_and_target(const Edge& that) const {
    return m_type == that.m_type &&
           ((m_throw_info == nullptr && that.m_throw_info == nullptr) ||
            *m_throw_info == *that.m_throw_info) &&
           m_case_key == that.m_case_key;
  }

  Block* src() const { return m_src; }
  Block* target() const { return m_target; }
  EdgeType type() const { return m_type; }
  ThrowInfo* throw_info() const { return m_throw_info.get(); }
  const boost::optional<CaseKey>& case_key() const { return m_case_key; }
  void set_case_key(boost::optional<CaseKey> k) { m_case_key = k; }
};

std::ostream& operator<<(std::ostream& os, const Edge& e);

using BlockId = size_t;

template <bool is_const>
class InstructionIteratorImpl;
using InstructionIterator = InstructionIteratorImpl</* is_const */ false>;
using ConstInstructionIterator = InstructionIteratorImpl</* is_const */ true>;

template <bool is_const>
class InstructionIterableImpl;
using InstructionIterable = InstructionIterableImpl</* is_const */ false>;
using ConstInstructionIterable = InstructionIterableImpl</* is_const */ true>;

// A piece of "straight-line" code. Targets are only at the beginning of a block
// and branches (throws, gotos, switches, etc) are only at the end of a block.
class Block final {
 public:
  explicit Block(ControlFlowGraph* parent, BlockId id)
      : m_id(id), m_parent(parent) {}

  ~Block() { m_entries.clear_and_dispose(); }

  // copy constructor
  Block(const Block& b, MethodItemEntryCloner* cloner);

  BlockId id() const { return m_id; }
  const std::vector<Edge*>& preds() const { return m_preds; }
  const std::vector<Edge*>& succs() const { return m_succs; }

  bool operator<(const Block& other) const { return this->id() < other.id(); }

  // return true if `b` is a predecessor of this.
  // optionally supply a specific type of predecessor. The default,
  // EDGE_TYPE_SIZE, means any type
  bool has_pred(Block* b, EdgeType t = EDGE_TYPE_SIZE) const;

  // return true if `b` is a successor of this.
  // optionally supply a specific type of successor. The default,
  // EDGE_TYPE_SIZE, means any type
  bool has_succ(Block* b, EdgeType t = EDGE_TYPE_SIZE) const;

  IRList::iterator begin();
  IRList::iterator end();
  IRList::const_iterator begin() const;
  IRList::const_iterator end() const;
  IRList::reverse_iterator rbegin() { return IRList::reverse_iterator(end()); }
  IRList::reverse_iterator rend() { return IRList::reverse_iterator(begin()); }

  bool is_catch() const;

  bool same_try(const Block* other) const;

  void remove_insn(const InstructionIterator& it);
  void remove_insn(const ir_list::InstructionIterator& it);
  void remove_insn(const IRList::iterator& it);

  opcode::Branchingness branchingness();

  // returns true if there are no MethodItemEntries (not IRInstructions)
  bool empty() const { return m_entries.empty(); }

  uint32_t num_opcodes() const;

  uint32_t sum_opcode_sizes() const;

  // return an iterator to the last MFLOW_OPCODE, or end() if there are none
  IRList::iterator get_last_insn();
  // return an iterator to the first MFLOW_OPCODE, or end() if there are none
  IRList::iterator get_first_insn();

  // including move-result-pseudo
  bool starts_with_move_result();

  // If this block has a single outgoing goto edge, return the target.
  // Otherwise, return nullptr
  Block* follow_goto() const;

  // TODO?: Should we just always store the throws in index order?
  std::vector<Edge*> get_outgoing_throws_in_order() const;

  // Remove the first target in this block that corresponds to `branch`.
  // Returns a not-none CaseKey for multi targets, boost::none otherwise.
  boost::optional<Edge::CaseKey> remove_first_matching_target(
      MethodItemEntry* branch);

  // These assume that the iterator is inside this block
  InstructionIterator to_cfg_instruction_iterator(
      const ir_list::InstructionIterator& list_it);
  InstructionIterator to_cfg_instruction_iterator(
      const IRList::iterator& list_it);
  InstructionIterator to_cfg_instruction_iterator(MethodItemEntry& mie);

  // These forward to implementations in ControlFlowGraph, See comment there
  bool insert_before(const InstructionIterator& position,
                     const std::vector<IRInstruction*>& insns);
  bool insert_before(const InstructionIterator& position, IRInstruction* insn);
  bool insert_after(const InstructionIterator& position,
                    const std::vector<IRInstruction*>& insns);
  bool insert_after(const InstructionIterator& position, IRInstruction* insn);
  bool push_front(const std::vector<IRInstruction*>& insns);
  bool push_front(IRInstruction* insn);
  bool push_back(const std::vector<IRInstruction*>& insns);
  bool push_back(IRInstruction* insn);

 private:
  friend class ControlFlowGraph;
  friend class CFGInliner;
  friend class InstructionIteratorImpl<false>;
  friend class InstructionIteratorImpl<true>;

  // return an iterator to the conditional branch (including switch) in this
  // block. If there is no such instruction, return end()
  IRList::iterator get_conditional_branch();

  BlockId m_id;

  // MethodItemEntries get moved from IRCode into here (if m_editable)
  // otherwise, this is empty.
  IRList m_entries;

  // TODO delete these
  // These refer into the IRCode IRList
  // These are only used in non-editable mode.
  IRList::iterator m_begin;
  IRList::iterator m_end;

  std::vector<Edge*> m_preds;
  std::vector<Edge*> m_succs;

  // the graph that this block belongs to
  ControlFlowGraph* m_parent = nullptr;
};

struct DominatorInfo {
  Block* dom;
  size_t postorder;
};

class ControlFlowGraph {

 public:
  static constexpr bool DEBUG{false};

  ControlFlowGraph() = default;
  ControlFlowGraph(const ControlFlowGraph&) = delete;

  /*
   * if editable is false, changes to the CFG aren't reflected in the output dex
   * instructions.
   */
  ControlFlowGraph(IRList* ir, uint16_t registers_size, bool editable = true);
  ~ControlFlowGraph();

  /*
   * convert from the graph representation to a list of MethodItemEntries
   */
  IRList* linearize();

  // TODO: this copies blocks, but it should probably offer a read-only view
  // into the blocks map instead.
  //
  // If a block is created or destroyed while we're iterating on a copy, the
  // copy is now stale. That stale copy may have a pointer to a deleted block or
  // it may be incomplete (not iterating over the newly creating block).
  //
  // A read-only view would be better because block creation and destruction
  // operations don't invalidate std::map iterators.
  std::vector<Block*> blocks() const;

  Block* create_block();
  const Block* entry_block() const { return m_entry_block; }
  const Block* exit_block() const { return m_exit_block; }
  Block* entry_block() { return m_entry_block; }
  Block* exit_block() { return m_exit_block; }
  void set_entry_block(Block* b) { m_entry_block = b; }
  void set_exit_block(Block* b) { m_exit_block = b; }

  /*
   * If there is a single method exit point, this returns a vector holding the
   * exit block. If there are multiple method exit points, this returns a vector
   * of them, ignoring the "ghost" exit block introduced by
   * `calculate_exit_block()`.
   */
  std::vector<Block*> real_exit_blocks(bool include_infinite_loops = false);

  std::vector<Block*> return_blocks() const;

  /*
   * Determine where the exit block is. If there is more than one, create a
   * "ghost" block that is the successor to all of them.
   *
   * The exit blocks are not computed upon creation. It is left up to the user
   * to call this method if they plan to use the exit block. If you make
   * significant changes to this CFG in editable mode that effect the exit
   * points of the method, you need to call this method again.
   * TODO: detect changes and recompute when necessary.
   */
  void calculate_exit_block();

  // args are arguments to an Edge constructor
  template <class... Args>
  void add_edge(Args&&... args) {
    add_edge(new Edge(std::forward<Args>(args)...));
  }

  void add_edge(Edge* e) {
    m_edges.insert(e);
    e->src()->m_succs.emplace_back(e);
    e->target()->m_preds.emplace_back(e);
  }

  using EdgeSet = std::unordered_set<Edge*>;

  // Make `e` point to a new target block.
  // The source block is unchanged.
  void set_edge_target(Edge* e, Block* new_target);

  // Make `e` come from a new source block
  // The target block is unchanged.
  void set_edge_source(Edge* e, Block* source_target);

  // return the first edge for which predicate returns true
  // or nullptr if no such edge exists
  template <typename EdgePredicate>
  Edge* get_pred_edge_if(const Block* block, EdgePredicate predicate) const {
    for (Edge* e : block->preds()) {
      if (predicate(e)) {
        return e;
      }
    }
    return nullptr;
  }

  template <typename EdgePredicate>
  Edge* get_succ_edge_if(const Block* block, EdgePredicate predicate) const {
    for (Edge* e : block->succs()) {
      if (predicate(e)) {
        return e;
      }
    }
    return nullptr;
  }

  // return all edges for which predicate returns true
  template <typename EdgePredicate>
  std::vector<Edge*> get_pred_edges_if(const Block* block,
                                       EdgePredicate predicate) const {
    const auto& preds = block->preds();
    std::vector<Edge*> result;
    for (Edge* e : preds) {
      if (predicate(e)) {
        result.push_back(e);
      }
    }
    return result;
  }

  template <typename EdgePredicate>
  std::vector<Edge*> get_succ_edges_if(const Block* block,
                                       EdgePredicate predicate) const {
    const auto& succs = block->succs();
    std::vector<Edge*> result;
    for (Edge* e : succs) {
      if (predicate(e)) {
        result.push_back(e);
      }
    }
    return result;
  }

  // return the first edge of the given type
  // or nullptr if no such edge exists
  Edge* get_pred_edge_of_type(const Block* block, EdgeType type) const;
  Edge* get_succ_edge_of_type(const Block* block, EdgeType type) const;

  // return all edges of the given type
  std::vector<Edge*> get_pred_edges_of_type(const Block* block,
                                            EdgeType type) const;
  std::vector<Edge*> get_succ_edges_of_type(const Block* block,
                                            EdgeType type) const;

  // delete_..._edge:
  //   * These functions remove edges from the graph and free the memory
  //   * the `_if` functions take a predicate to decide which edges to delete
  void delete_edge(Edge* edge);
  void delete_succ_edges(Block* b);
  void delete_pred_edges(Block* b);
  void delete_edges_between(Block* p, Block* s);

  template <typename EdgePredicate>
  void delete_edge_if(Block* source, Block* target, EdgePredicate predicate) {
    free_edges(remove_edge_if(source, target, predicate));
  }

  template <typename EdgePredicate>
  void delete_succ_edge_if(Block* block, EdgePredicate predicate) {
    free_edges(remove_succ_edge_if(block, predicate));
  }

  template <typename EdgePredicate>
  void delete_pred_edge_if(Block* block, EdgePredicate predicate) {
    free_edges(remove_pred_edge_if(block, predicate));
  }

  bool blocks_are_in_same_try(const Block* b1, const Block* b2) const;

  /*
   * Split this block into two blocks. After this call, `it` will be the last
   * instruction in the predecessor block.
   *
   * The existing block will become the predecessor. All code after `it` will be
   * moved into the new block (the successor). Return the (new) successor.
   */
  Block* split_block(const cfg::InstructionIterator& it);

  // Merge `succ` into `pred` and delete `succ`
  //
  // `pred` must be the only predecessor of `succ`
  // `succ` must be the only successor of `pred`
  // `pred` and `succ` must be in the same try region
  void merge_blocks(Block* pred, Block* succ);

  // remove the IRInstruction that `it` points to.
  //
  // If `it` points to a branch instruction, remove the corresponding outgoing
  // edges.
  void remove_insn(const InstructionIterator& it);

  // Insertion Methods (insert_before/after and push_front/back):
  //  * These methods add instructions to the CFG
  //  * They do not add branch (if-*, switch-*) instructions to the cfg (use
  //    create_branch for that)
  //  * If the inserted instruction requires a block boundary after it, the
  //    block will be split, instructions will be moved to the next
  //    (non-exceptional) block and the next insertion from `insns` will also
  //    occur in the next block. This invalidates iterators into the CFG.
  //  * If the inserted instruction could end the method (return-* or throw),
  //    then instructions after the insertion point will be removed and
  //    successor edges will be removed from the block. When inserting a return
  //    or throw, it must be the last in `insns`. This invalidates
  //    iterators into the CFG.
  //
  // insert insns before position
  // return a boolean:
  //   true means that iterators into the CFG are now invalid
  //   false means that iterators are still valid
  bool insert_before(const InstructionIterator& position,
                     const std::vector<IRInstruction*>& insns);
  // insert insns after position
  bool insert_after(const InstructionIterator& position,
                    const std::vector<IRInstruction*>& insns);
  // insert insns at the beginning of block b
  bool push_front(Block* b, const std::vector<IRInstruction*>& insns);
  // insert insns at the end of block b
  bool push_back(Block* b, const std::vector<IRInstruction*>& insns);

  // Convenience functions that add just one instruction.
  bool insert_before(const InstructionIterator& position, IRInstruction* insn);
  bool insert_after(const InstructionIterator& position, IRInstruction* insn);
  bool push_front(Block* b, IRInstruction* insn);
  bool push_back(Block* b, IRInstruction* insn);

  // Create a conditional branch, which consists of:
  // * inserting an `if` instruction at the end of b
  // * Possibly add an EDGE_GOTO to the false block (`fls` may be null if b
  //   already has a goto leaving it)
  // * add an EDGE_BRANCH to the true block
  void create_branch(Block* b, IRInstruction* insn, Block* fls, Block* tru);

  // Create an `if` or `switch`, which consists of:
  // * insert an `if` or `switch` instruction at the end of b
  // * Possibly add an EDGE_GOTO to the default block (`goto_block` may be null
  //   if b already has a goto leaving it)
  // * add EDGE_BRANCHes to the other blocks based on the `case_to_block`
  //   vector.
  void create_branch(
      Block* b,
      IRInstruction* insn,
      Block* goto_block,
      const std::vector<std::pair<int32_t, Block*>>& case_to_block);

  // delete old_block and reroute its predecessors to new_block
  void replace_block(Block* old_block, Block* new_block);

  // Remove this block from the graph and release associated memory.
  // Remove all incoming and outgoing edges.
  void remove_block(Block* block);

  /*
   * Print the graph in the DOT graph description language.
   */
  std::ostream& write_dot_format(std::ostream&) const;

  // Find a common dominator block that is closest to both block.
  Block* idom_intersect(
      const std::unordered_map<Block*, DominatorInfo>& postorder_dominator,
      Block* block1,
      Block* block2) const;

  // Finding immediate dominator for each blocks in ControlFlowGraph.
  std::unordered_map<Block*, DominatorInfo> immediate_dominators() const;

  // Do writes to this CFG propagate back to IR and Dex code?
  bool editable() const { return m_editable; }

  size_t num_blocks() const { return m_blocks.size(); }

  /*
   * Traverse the graph, starting from the entry node. Return a bitset with IDs
   * of reachable blocks having 1 and IDs of unreachable blocks (or unused IDs)
   * having 0.
   */
  boost::dynamic_bitset<> visit() const;

  // remove blocks with no predecessors
  // returns the number of instructions removed
  uint32_t remove_unreachable_blocks();

  // transform the CFG to an equivalent but more canonical state
  // Assumes m_editable is true
  void simplify();

  // SIGABORT if the internal state of the CFG is invalid
  void sanity_check() const;

  // SIGABORT if there are dangling parent pointers to deleted DexPositions
  void no_dangling_dex_positions() const;

  uint32_t num_opcodes() const;

  uint32_t sum_opcode_sizes() const;

  uint16_t allocate_temp() { return m_registers_size++; }

  uint16_t allocate_wide_temp() {
    uint16_t new_reg = m_registers_size;
    m_registers_size += 2;
    return new_reg;
  }

  uint16_t get_registers_size() const { return m_registers_size; }

  void set_registers_size(uint16_t sz) { m_registers_size = sz; }

  // Find the highest register in use and set m_registers_size
  //
  // Call this function after removing instructions that may have been the only
  // use of the highest numbered register, or any other significant changes to
  // the instructions.
  void recompute_registers_size();

  // by default, start at the entry block
  boost::sub_range<IRList> get_param_instructions();

  void gather_catch_types(std::vector<DexType*>& types) const;
  void gather_strings(std::vector<DexString*>& strings) const;
  void gather_types(std::vector<DexType*>& types) const;
  void gather_fields(std::vector<DexFieldRef*>& fields) const;
  void gather_methods(std::vector<DexMethodRef*>& methods) const;

  cfg::InstructionIterator move_result_of(const cfg::InstructionIterator& it);

  /*
   * fill `new_cfg` with a copy of `this`
   */
  void deep_copy(ControlFlowGraph* new_cfg) const;

  // Search all the instructions in this CFG for the given one. Return an
  // iterator to it, or end, if it isn't in the graph.
  InstructionIterator find_insn(IRInstruction* insn);

  // choose an order of blocks for output
  std::vector<Block*> order();

 private:
  using BranchToTargets =
      std::unordered_map<MethodItemEntry*, std::vector<Block*>>;
  using TryEnds = std::vector<std::pair<TryEntry*, Block*>>;
  using TryCatches = std::unordered_map<CatchEntry*, Block*>;
  using Blocks = std::map<BlockId, Block*>;
  friend class InstructionIteratorImpl<false>;
  friend class InstructionIteratorImpl<true>;
  friend class CFGInliner;

  // Find block boundaries in IRCode and create the blocks
  // For use by the constructor. You probably don't want to call this from
  // elsewhere
  void find_block_boundaries(IRList* ir,
                             BranchToTargets& branch_to_targets,
                             TryEnds& try_ends,
                             TryCatches& try_catches);

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

  // remove all MFLOW_TRY and MFLOW_CATCH markers because that information is
  // moved into the edges.
  // Assumes m_editable is true
  void remove_try_catch_markers();

  // helper functions
  using Chain = std::vector<Block*>;
  void build_chains(std::vector<std::unique_ptr<Chain>>* chains,
                    std::unordered_map<Block*, Chain*>* block_to_chain);
  std::vector<Block*> wto_chains(
      const std::unordered_map<Block*, Chain*>& block_to_chain);

  // Materialize target instructions and gotos corresponding to control-flow
  // edges. Used while turning back into a linear representation.
  void insert_branches_and_targets(const std::vector<Block*>& ordering);

  // Create MFLOW_CATCH entries for each EDGE_THROW. returns the primary
  // MFLOW_CATCH (first element in linked list of CatchEntries) on a block that
  // can throw. returns nullptr on a block without outgoing throw edges. This
  // function is used while turning back into a linear representation.
  //
  // Example: For a block with outgoing edges
  //
  // Edge(block, catch_block_1, FooException, 1)
  // Edge(block, catch_block_2, BarException, 2)
  // Edge(block, catch_block_3, BazException, 3)
  //
  // This generates CatchEntries (linked list)
  //
  // CatchEntry(FooException) ->
  //   CatchEntry(BarException) ->
  //     CatchEntry(BazException)
  MethodItemEntry* create_catch(
      Block* block,
      std::unordered_map<MethodItemEntry*, Block*>* catch_to_containing_block);

  // Materialize TRY_STARTs, TRY_ENDs, and MFLOW_CATCHes
  // Used while turning back into a linear representation.
  void insert_try_catch_markers(const std::vector<Block*>& ordering);

  // Follow the catch entry linked list starting at `first_mie` and
  // make sure the throw edges (pointed to by `it`) agree with the linked list.
  // Used while turning back into a linear representation.
  bool catch_entries_equivalent_to_throw_edges(
      MethodItemEntry* first_mie,
      std::vector<Edge*>::iterator it,
      std::vector<Edge*>::iterator end,
      const std::unordered_map<MethodItemEntry*, Block*>&
          catch_to_containing_block);

  // remove blocks with no entries
  void remove_empty_blocks();

  // remove any parent pointer that was passed in as an arg (e.g. for when
  // you delete the supplied positions)
  void remove_dangling_parents(const std::unordered_set<DexPosition*>&);

  // Assert if there are edges that are never a predecessor or successor of a
  // block
  void no_unreferenced_edges() const;

  bool insert(const InstructionIterator& position,
              const std::vector<IRInstruction*>& insns,
              bool before);

  // remove_..._edge:
  //   * These functions remove edges from the graph.
  //   * They do not free the memory of the edge. `free_edge` does that.
  //   * The cleanup flag controls whether or not `cleanup_deleted_edges` is
  //     called. See that function for more documentation.
  //   * the `_if` functions take a predicate to decide which edges to delete
  //   * They return which edges were removed (with the exception of
  //     `remove_edge`)
  void remove_edge(Edge* edge, bool cleanup = true);
  EdgeSet remove_succ_edges(Block* b, bool cleanup = true);
  EdgeSet remove_pred_edges(Block* b, bool cleanup = true);
  EdgeSet remove_edges_between(Block* p, Block* s, bool cleanup = true);

  template <typename EdgePredicate>
  EdgeSet remove_edge_if(Block* source,
                         Block* target,
                         EdgePredicate predicate,
                         bool cleanup = true) {
    auto& forward_edges = source->m_succs;
    EdgeSet to_remove;
    forward_edges.erase(
        std::remove_if(forward_edges.begin(),
                       forward_edges.end(),
                       [&target, &predicate, &to_remove](Edge* e) {
                         if (e->target() == target && predicate(e)) {
                           to_remove.insert(e);
                           return true;
                         }
                         return false;
                       }),
        forward_edges.end());

    auto& reverse_edges = target->m_preds;
    reverse_edges.erase(std::remove_if(reverse_edges.begin(),
                                       reverse_edges.end(),
                                       [&to_remove](Edge* e) {
                                         return to_remove.count(e) > 0;
                                       }),
                        reverse_edges.end());

    if (cleanup) {
      cleanup_deleted_edges(to_remove);
    }
    return to_remove;
  }

  template <typename EdgePredicate>
  EdgeSet remove_pred_edge_if(Block* block,
                              EdgePredicate predicate,
                              bool cleanup = true) {
    auto& reverse_edges = block->m_preds;

    std::vector<Block*> source_blocks;
    EdgeSet to_remove;
    reverse_edges.erase(
        std::remove_if(reverse_edges.begin(),
                       reverse_edges.end(),
                       [&source_blocks, &to_remove, &predicate](Edge* e) {
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
      forward_edges.erase(
          std::remove_if(
              forward_edges.begin(), forward_edges.end(),
              [&to_remove](Edge* e) { return to_remove.count(e) > 0; }),
          forward_edges.end());
    }

    if (cleanup) {
      cleanup_deleted_edges(to_remove);
    }
    return to_remove;
  }

  template <typename EdgePredicate>
  EdgeSet remove_succ_edge_if(Block* block,
                              EdgePredicate predicate,
                              bool cleanup = true) {
    auto& forward_edges = block->m_succs;

    std::vector<Block*> target_blocks;
    std::unordered_set<Edge*> to_remove;
    forward_edges.erase(
        std::remove_if(forward_edges.begin(),
                       forward_edges.end(),
                       [&target_blocks, &to_remove, &predicate](Edge* e) {
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
      reverse_edges.erase(
          std::remove_if(
              reverse_edges.begin(), reverse_edges.end(),
              [&to_remove](Edge* e) { return to_remove.count(e) > 0; }),
          reverse_edges.end());
    }

    if (cleanup) {
      cleanup_deleted_edges(to_remove);
    }
    return to_remove;
  }

  // Assumes the edge is already removed.
  void free_edge(Edge* edge);

  // Assumes the edge is already removed.
  void free_edges(const EdgeSet& edges);

  // The `cleanup` boolean flag on the edge removal functions controls whether
  // or not to call this function afterwards.
  //   * `cleanup` false means only remove the edges
  //   * `cleanup` true means remove the edges and edit the instructions to
  //      match the edge state. For example, delete branch/switch with only one
  //      outgoing edge instructions.
  void cleanup_deleted_edges(const EdgeSet& edges);

  // Move edge between new_source and new_target.
  // If either new_source or new_target is null, don't change that field of the
  // edge
  void move_edge(Edge* edge, Block* new_source, Block* new_target);

  uint16_t compute_registers_size() const;

  // Return the next unused block identifier
  BlockId next_block_id() const;

  // The memory of all blocks and edges in this graph are owned here
  Blocks m_blocks;
  EdgeSet m_edges;

  uint16_t m_registers_size{0};
  Block* m_entry_block{nullptr};
  Block* m_exit_block{nullptr};
  bool m_editable{true};
};

// A static-method-only API for use with the monotonic fixpoint iterator.
class GraphInterface {

 public:
  using Graph = ControlFlowGraph;
  using NodeId = Block*;
  using EdgeId = Edge*;
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

template <bool is_const>
class InstructionIteratorImpl {
  using Cfg = typename std::
      conditional<is_const, const ControlFlowGraph, ControlFlowGraph>::type;
  using Mie = typename std::
      conditional<is_const, const MethodItemEntry, MethodItemEntry>::type;
  using Iterator = typename std::
      conditional<is_const, IRList::const_iterator, IRList::iterator>::type;

  // Use a pointer so that we can be copy constructible
  Cfg* m_cfg;
  ControlFlowGraph::Blocks::const_iterator m_block;

  // Depends on C++14 Null Forward Iterators
  // Assuming the default constructed InstructionIterator compares equal
  // to other default constructed InstructionIterator
  //
  // boost.org/doc/libs/1_58_0/doc/html/container/Cpp11_conformance.html
  ir_list::InstructionIteratorImpl<is_const> m_it;

  // go to beginning of next block, skipping empty blocks
  void to_next_block() {
    while (m_block != m_cfg->m_blocks.end() &&
           m_it.unwrap() == m_block->second->m_entries.end()) {
      ++m_block;
      if (m_block != m_cfg->m_blocks.end()) {
        Block* b = m_block->second;
        m_it = ir_list::InstructionIteratorImpl<is_const>(b->m_entries.begin(),
                                                          b->m_entries.end());
      } else {
        m_it = ir_list::InstructionIteratorImpl<is_const>();
      }
    }
  }

  friend class ControlFlowGraph;
  friend class Block;
  InstructionIteratorImpl(Cfg& cfg,
                          Block* b,
                          const ir_list::InstructionIterator& it)
      : m_cfg(&cfg), m_block(m_cfg->m_blocks.find(b->id())), m_it(it) {}

 public:
  using reference = Mie&;
  using difference_type = long;
  using value_type = Mie&;
  using pointer = Mie*;
  using iterator_category = std::forward_iterator_tag;

  // TODO: Is it possible to recover a valid state of iterators into the CFG
  // after an insertion operation?
  //
  // The goal is to maintain a valid state within the CFG at all times. If the
  // user wants to insert an instruction that ends the block (return, can_throw,
  // if, switch, etc.), the block needs to split. When you split a block into
  // two parts, you're moving code from one block to another. When code is moved
  // `InstructionIterator`s may be left in an invalid state because their `m_it`
  // is pointing into a different block than `m_block`.

  InstructionIteratorImpl() = delete;

  explicit InstructionIteratorImpl(Cfg& cfg, bool is_begin) : m_cfg(&cfg) {
    always_assert(m_cfg->editable());
    if (is_begin) {
      m_block = m_cfg->m_blocks.begin();
      if (m_block != m_cfg->m_blocks.end()) {
        auto iterable =
            ir_list::InstructionIterableImpl<is_const>(m_block->second);
        m_it = iterable.begin();
        if (m_it == iterable.end()) {
          to_next_block();
        }
      }
    } else {
      m_block = m_cfg->m_blocks.end();
    }
  }

  InstructionIteratorImpl<is_const>& operator++() {
    assert_not_end();
    ++m_it;
    to_next_block();
    return *this;
  }

  InstructionIteratorImpl<is_const> operator++(int) {
    auto result = *this;
    ++(*this);
    return result;
  }

  reference operator*() const {
    assert_not_end();
    return *m_it;
  }

  pointer operator->() const { return &(this->operator*()); }

  bool operator==(const InstructionIteratorImpl& other) const {
    return this->m_block == other.m_block && this->m_it == other.m_it;
  }

  bool operator!=(const InstructionIteratorImpl& other) const {
    return !(*this == other);
  }

  void assert_not_end() const {
    if (!ControlFlowGraph::DEBUG) {
      return;
    }
    always_assert_log(m_block != m_cfg->m_blocks.end(), "%s", SHOW(*m_cfg));
    always_assert_log(m_it != ir_list::InstructionIteratorImpl<is_const>(),
                      "%s", SHOW(*m_cfg));
  }

  bool is_end() const {
    return m_block == m_cfg->m_blocks.end() &&
           m_it == ir_list::InstructionIteratorImpl<is_const>();
  }

  Iterator unwrap() const { return m_it.unwrap(); }

  Block* block() const {
    assert_not_end();
    return m_block->second;
  }

  Cfg& cfg() const { return *m_cfg; }
};

// Iterate through all IRInstructions in the CFG.
// Instructions in the same block are processed in order.
// Blocks are iterated in an undefined order
template <bool is_const>
class InstructionIterableImpl {
  using Cfg = typename std::
      conditional<is_const, const ControlFlowGraph, ControlFlowGraph>::type;
  using Iterator = typename std::
      conditional<is_const, IRList::const_iterator, IRList::iterator>::type;
  Cfg& m_cfg;

 public:
  InstructionIterableImpl() = delete;
  explicit InstructionIterableImpl(Cfg& cfg) : m_cfg(cfg) {}

  InstructionIteratorImpl<is_const> begin() {
    return InstructionIteratorImpl<is_const>(m_cfg, true);
  }

  InstructionIteratorImpl<is_const> end() {
    return InstructionIteratorImpl<is_const>(m_cfg, false);
  }

  bool empty() { return begin() == end(); }
};

/*
 * Build a postorder sorted vector of blocks from the given CFG. Uses a
 * standard depth-first search with a side table of already-visited nodes.
 */
std::vector<Block*> postorder_sort(const std::vector<Block*>& cfg);

} // namespace cfg

inline cfg::InstructionIterable InstructionIterable(
    cfg::ControlFlowGraph& cfg) {
  return cfg::InstructionIterable(cfg);
}

inline cfg::ConstInstructionIterable InstructionIterable(
    const cfg::ControlFlowGraph& cfg) {
  return cfg::ConstInstructionIterable(cfg);
}
