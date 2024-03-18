/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "DexPosition.h"
#include "IRInstruction.h"

#include <unordered_map>
#include <vector>

struct SourceBlock;

namespace cfg {

/// Gathers requests to insert \c IRInstructions into a \c ControlFlowGraph that
/// can be flushed out in batches.  This offers an alternative to modifying the
/// IR in a CFG whilst iterating over its instructions which is not supported in
/// general as a modification to the IR could invalidate the iterator.
///
/// TODO(T59235117) Flush mutation in the destructor.
class CFGMutation {
 public:
  /// Create a new mutation to apply to \p cfg.
  explicit CFGMutation(ControlFlowGraph& cfg);

  /// CFGMutation is not copyable
  CFGMutation(const CFGMutation&) = delete;
  CFGMutation& operator=(const CFGMutation&) = delete;

  /// CFGMutation is move-constructable
  CFGMutation(CFGMutation&&) = default;

  /// CFGMutation is not move-assignable.
  CFGMutation& operator=(CFGMutation&&) = delete;

  /// Add a new change to this mutation.
  /// Mutation may have multiple changes associated with \c anchor.
  /// Here is the resulting order of instructions applying multiple changes
  /// to a single \c anchor it.
  ///
  /// insert_before(it, as)
  /// replace(it, rs)
  /// insert_before(it, bs)
  /// insert_after(it, ys)
  /// insert_after(it, zs)
  ///
  /// as ++ bs ++ rs ++ ys ++ zs

  /// Insert a new change before \p anchor.
  /// Mutation may have multiple changes associated with \c anchor.
  ///     insert_before preserves the anchor instruction and adds the
  ///     instructions before it.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored.
  /// \p instructions are the instructions that are inserted as part of the
  ///     change. This can be an empty list.
  void insert_before(const cfg::InstructionIterator& anchor,
                     std::vector<IRInstruction*> instructions);

  /// Insert a new change before \p anchor.
  /// Mutation may have multiple changes associated with \c anchor.
  ///     insert_before preserves the anchor instruction and adds the
  ///     instructions before it.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored.
  /// \p instructions are the instructions that are inserted as part of the
  ///     change. This can be an empty list.
  void insert_before_var(
      const cfg::InstructionIterator& anchor,
      std::vector<cfg::ControlFlowGraph::InsertVariant> instructions);

  /// Insert a new change before \p anchor.
  /// Mutation may have multiple changes associated with \c anchor.
  ///     insert_before preserves the anchor instruction and adds the
  ///     position before it.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored.
  /// \p position is the debug position that is inserted as part of the
  ///     change.
  void insert_before(const cfg::InstructionIterator& anchor,
                     std::unique_ptr<DexPosition> position);

  /// Insert a new change before \p anchor.
  /// Mutation may have multiple changes associated with \c anchor.
  ///     insert_before preserves the anchor instruction and adds the
  ///     position before it.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored.
  /// \p sb is the source block that is inserted as part of the
  ///     change.
  void insert_before(const cfg::InstructionIterator& anchor,
                     std::unique_ptr<SourceBlock> sb);

  /// Insert a new change after \p anchor.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored. This preserves the anchor instruction and adds the
  ///     instructions after it.
  /// \p instructions are the instructions that are inserted as part of the
  ///     change.
  /// Mutation restrictions:
  ///  - It's not possible to insert_after terminal operation without
  ///    replacing it. This can be an empty list.
  void insert_after(const cfg::InstructionIterator& anchor,
                    std::vector<IRInstruction*> instructions);

  /// Insert a new change after \p anchor.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored. This preserves the anchor instruction and adds the
  ///     instructions after it.
  /// \p instructions are the instructions that are inserted as part of the
  ///     change.
  /// Mutation restrictions:
  ///  - It's not possible to insert_after terminal operation without
  ///    replacing it. This can be an empty list.
  void insert_after_var(
      const cfg::InstructionIterator& anchor,
      std::vector<cfg::ControlFlowGraph::InsertVariant> instructions);

  /// Insert a new change after \p anchor.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored. This preserves the anchor instruction and adds the
  ///     position after it.
  /// \p position is the debug position that is inserted as part of the
  ///     change.
  void insert_after(const cfg::InstructionIterator& anchor,
                    std::unique_ptr<DexPosition> position);

  /// Insert a new change after \p anchor.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored. This preserves the anchor instruction and adds the
  ///     position after it.
  /// \p sb is the source block that is inserted as part of the
  ///     change.
  void insert_after(const cfg::InstructionIterator& anchor,
                    std::unique_ptr<SourceBlock> sb);

  /// Replace with a new change at this \p anchor.
  /// replace behaves like either Before or After followed by removing the
  /// anchor instruction.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored.
  /// \p instructions are the instructions that are inserted as part of the
  ///     change. This can be an empty list.
  /// Mutation restrictions:
  ///  - It's not possible to have two replacing instructions for a single
  ///    anchor.
  /// Any removed instruction will be freed when the cfg is destroyed.
  void replace(const cfg::InstructionIterator& anchor,
               std::vector<IRInstruction*> instructions);

  /// Remove change at this \p anchor.
  /// \p anchor is the instruction that is removed. If at the time the change
  ///     is applied, the anchor does not exist, the change will be ignored.
  ///  - It's not possible to have two remove instructions for a single anchor.
  /// Any removed instruction will be freed when the cfg is destroyed.
  void remove(const cfg::InstructionIterator& anchor);

  /// Remove all pending changes without applying them.
  void clear();

  /// Apply all the changes that have been added since the last flush or clear
  /// (or since the mutation was created).  Changes are applied in the order
  /// they are added to the mutation.
  void flush();

 private:
  static bool is_terminal(IROpcode op);

  /// A memento of a change we wish to make to the CFG.
  class ChangeSet {
   public:
    explicit ChangeSet(const IRList::iterator& it) : m_it(it) {}

    enum class Insert { Before, After, Replacing };
    /// Apply this change on the control flow graph \p cfg, using \p it as the
    /// anchoring instruction. Moves \p it if the change invalidates the anchor.
    ///
    ///  - The iterator is guaranteed not to be moved past the first instruction
    ///    after the anchor's initial position.
    ///  - Note the iterator may not be moved at all, even if the change is
    ///    applied.
    /// Returns whether iterators were invalidated.
    bool apply(ControlFlowGraph& cfg,
               cfg::Block* block,
               ir_list::InstructionIterator& it);

    void scan(bool* throws_or_returns, bool* may_throw);

    /// Accumulates changes for a specific instruction.
    /// Check \link CFGMutation::add_change \endlink for more details
    void add_change(Insert where, std::vector<IRInstruction*> insn_change);

    /// Accumulates position changes for a specific instruction.
    void add_position(Insert where, std::unique_ptr<DexPosition> pos_change);

    /// Accumulates source block changes for a specific instruction.
    void add_source_block(Insert where, std::unique_ptr<SourceBlock> sb_change);

    void add_var(Insert where, cfg::ControlFlowGraph::InsertVariant var);
    void add_var(Insert where,
                 std::vector<cfg::ControlFlowGraph::InsertVariant> var);

    /// Free the instructions owned by this ChangeSet.  Leaves the ChangeSet
    /// empty (so applying it would be a nop).
    void dispose();

    /// Gets the original iterator.
    IRList::iterator& get_iterator() { return m_it; }

    bool has_replace() const { return !!m_replace; }

    bool is_simple_empty_replace() const {
      return m_insert_before.empty() && m_replace && m_replace->empty() &&
             m_insert_after.empty() && m_insert_pos_before.empty() &&
             m_insert_pos_after.empty() && m_insert_sb_before.empty() &&
             m_insert_sb_after.empty() && m_insert_before_var.empty() &&
             m_insert_after_var.empty();
    }

   private:
    IRList::iterator m_it;

    std::vector<IRInstruction*> m_insert_before;
    boost::optional<std::vector<IRInstruction*>> m_replace;
    std::vector<IRInstruction*> m_insert_after;

    std::vector<std::unique_ptr<DexPosition>> m_insert_pos_before;
    std::vector<std::unique_ptr<DexPosition>> m_insert_pos_after;

    std::vector<std::unique_ptr<SourceBlock>> m_insert_sb_before;
    std::vector<std::unique_ptr<SourceBlock>> m_insert_sb_after;

    std::vector<cfg::ControlFlowGraph::InsertVariant> m_insert_before_var;
    std::vector<cfg::ControlFlowGraph::InsertVariant> m_insert_after_var;
  };

  using Changes =
      std::unordered_map<IRInstruction*, std::unique_ptr<ChangeSet>>;

  ChangeSet* get_change_set(const cfg::InstructionIterator& anchor) {
    auto& change_set = m_changes[anchor.block()][anchor->insn];
    if (!change_set) {
      change_set = std::make_unique<ChangeSet>(anchor.unwrap());
    }
    return change_set.get();
  }

  ChangeSet* primary_change_of_move_result(cfg::Block* block,
                                           ChangeSet& move_result_change);

  /// Removes (and logs) move-result-any* changes that overlap with primary
  /// instruction changes. The return value indicates whether we need to do slow
  /// (sequential) processing, as some changes may invalidate iterators or add
  /// new blocks.
  bool reduce_block_changes(cfg::Block* block,
                            Changes& changes,
                            bool* requires_slow_processing);
  /// Apply changes by iterating over all instructions. The return value
  /// indicates whether all changes were processed.
  bool process_block_changes_slow(cfg::Block* block, Changes& changes);
  /// Apply changes in any order.
  void process_block_changes(cfg::Block* block, Changes& changes);

  cfg::ControlFlowGraph& m_cfg;
  std::unordered_map<cfg::Block*, Changes> m_changes;
};

inline CFGMutation::CFGMutation(cfg::ControlFlowGraph& cfg) : m_cfg(cfg) {}

inline void CFGMutation::ChangeSet::add_change(
    Insert where, std::vector<IRInstruction*> insn_change) {

  switch (where) {
  case Insert::Before:
    m_insert_before.insert(
        m_insert_before.end(), insn_change.begin(), insn_change.end());
    break;
  case Insert::After:
    m_insert_after.insert(
        m_insert_after.end(), insn_change.begin(), insn_change.end());
    break;
  case Insert::Replacing:
    always_assert_log(!m_replace.has_value(),
                      "It's not possible to have two Replacing instructions "
                      "for a single anchor.");
    m_replace = std::move(insn_change);
    break;
  }
}

inline void CFGMutation::ChangeSet::add_position(
    Insert where, std::unique_ptr<DexPosition> pos_change) {

  switch (where) {
  case Insert::Before:
    m_insert_pos_before.emplace_back(std::move(pos_change));
    break;
  case Insert::After:
    m_insert_pos_after.emplace_back(std::move(pos_change));
    break;
  case Insert::Replacing:
    always_assert_log(false, "Cannot replace dex positions.");
    break;
  }
}

inline void CFGMutation::ChangeSet::add_source_block(
    Insert where, std::unique_ptr<SourceBlock> sb_change) {
  switch (where) {
  case Insert::Before:
    m_insert_sb_before.emplace_back(std::move(sb_change));
    break;
  case Insert::After:
    m_insert_sb_after.emplace_back(std::move(sb_change));
    break;
  case Insert::Replacing:
    always_assert_log(false, "Cannot replace dex positions.");
    break;
  }
}

inline void CFGMutation::ChangeSet::add_var(
    Insert where, cfg::ControlFlowGraph::InsertVariant var) {
  switch (where) {
  case Insert::Before:
    m_insert_before_var.emplace_back(std::move(var));
    break;
  case Insert::After:
    m_insert_after_var.emplace_back(std::move(var));
    break;
  case Insert::Replacing:
    always_assert_log(false, "Unsupported.");
    break;
  }
}
inline void CFGMutation::ChangeSet::add_var(
    Insert where, std::vector<cfg::ControlFlowGraph::InsertVariant> var) {
  switch (where) {
  case Insert::Before:
    for (auto& v : var) {
      m_insert_before_var.emplace_back(std::move(v));
    }
    break;
  case Insert::After:
    for (auto& v : var) {
      m_insert_after_var.emplace_back(std::move(v));
    }
    break;
  case Insert::Replacing:
    always_assert_log(false, "Unsupported.");
    break;
  }
}

inline void CFGMutation::insert_before(
    const cfg::InstructionIterator& anchor,
    std::vector<IRInstruction*> instructions) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_change(ChangeSet::Insert::Before,
                                     std::move(instructions));
}

inline void CFGMutation::insert_after(
    const cfg::InstructionIterator& anchor,
    std::vector<IRInstruction*> instructions) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_change(ChangeSet::Insert::After,
                                     std::move(instructions));
}

inline void CFGMutation::insert_before_var(
    const cfg::InstructionIterator& anchor,
    std::vector<cfg::ControlFlowGraph::InsertVariant> instructions) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_var(ChangeSet::Insert::Before,
                                  std::move(instructions));
}

inline void CFGMutation::insert_after_var(
    const cfg::InstructionIterator& anchor,
    std::vector<cfg::ControlFlowGraph::InsertVariant> instructions) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_var(ChangeSet::Insert::After,
                                  std::move(instructions));
}

inline void CFGMutation::insert_before(const cfg::InstructionIterator& anchor,
                                       std::unique_ptr<DexPosition> position) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_position(ChangeSet::Insert::Before,
                                       std::move(position));
}

inline void CFGMutation::insert_after(const cfg::InstructionIterator& anchor,
                                      std::unique_ptr<DexPosition> position) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_position(ChangeSet::Insert::After,
                                       std::move(position));
}

inline void CFGMutation::insert_before(const cfg::InstructionIterator& anchor,
                                       std::unique_ptr<SourceBlock> sb) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_source_block(ChangeSet::Insert::Before,
                                           std::move(sb));
}

inline void CFGMutation::insert_after(const cfg::InstructionIterator& anchor,
                                      std::unique_ptr<SourceBlock> sb) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_source_block(ChangeSet::Insert::After,
                                           std::move(sb));
}

inline void CFGMutation::replace(const cfg::InstructionIterator& anchor,
                                 std::vector<IRInstruction*> instructions) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_change(ChangeSet::Insert::Replacing,
                                     std::move(instructions));
}

inline void CFGMutation::remove(const cfg::InstructionIterator& anchor) {
  always_assert(!anchor.is_end());
  get_change_set(anchor)->add_change(ChangeSet::Insert::Replacing, {});
}

inline bool CFGMutation::is_terminal(IROpcode op) {
  return opcode::is_branch(op) || opcode::is_throw(op) ||
         opcode::is_a_return(op);
}

} // namespace cfg
