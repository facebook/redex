/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "IRInstruction.h"

#include <unordered_map>
#include <vector>

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
  ///     insert_before preserves the anchor instruction and add the
  ///     instructions before it.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored.
  /// \p instructions are the instructions that are inserted as part of the
  ///     change. This can be an empty list.
  void insert_before(const cfg::InstructionIterator& anchor,
                     std::vector<IRInstruction*> instructions);

  /// Insert a new change after \p anchor.
  /// \p anchor is the instruction that the change is made relative to. If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored. This preserve the anchor instruction and add the
  ///     instructions after it.
  /// \p instructions are the instructions that are inserted as part of the
  ///     change.
  /// Mutation restrictions:
  ///  - It's not possible to insert_after terminal operation without
  ///    replacing it. This can be an empty list.
  void insert_after(const cfg::InstructionIterator& anchor,
                    std::vector<IRInstruction*> instructions);

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
  void replace(const cfg::InstructionIterator& anchor,
               std::vector<IRInstruction*> instructions);

  /// Remove change at this \p anchor.
  /// \p anchor is the instruction that is removed. If at the time the change
  ///     is applied, the anchor does not exist, the change will be ignored.
  ///  - It's not possible to have two remove instructions for a single anchor.
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
    enum class Insert { Before, After, Replacing };
    /// Apply this change on the control flow graph \p cfg, using \p it as the
    /// anchoring instruction. Moves \p it if the change invalidates the anchor.
    ///
    ///  - The iterator is guaranteed not to be moved past the first instruction
    ///    after the anchor's initial position.
    ///  - Note the iterator may not be moved at all, even if the change is
    ///    applied.
    void apply(ControlFlowGraph& cfg, InstructionIterator& it) const;

    /// Accumulates changes for a specific instruction.
    /// Check \link CFGMutation::add_change \endlink for more details
    void add_change(Insert where, std::vector<IRInstruction*> insn_change);

    /// Free the instructions owned by this ChangeSet.  Leaves the ChangeSet
    /// empty (so applying it would be a nop).
    void dispose();

   private:
    std::vector<IRInstruction*> m_insert_before;
    boost::optional<std::vector<IRInstruction*>> m_replace;
    std::vector<IRInstruction*> m_insert_after;
  };

  cfg::ControlFlowGraph& m_cfg;
  std::unordered_map<IRInstruction*, ChangeSet> m_changes;
};

inline CFGMutation::CFGMutation(cfg::ControlFlowGraph& cfg) : m_cfg(cfg) {}

inline void CFGMutation::ChangeSet::add_change(
    Insert where, std::vector<IRInstruction*> insns) {

  switch (where) {
  case Insert::Before:
    m_insert_before.insert(m_insert_before.end(), insns.begin(), insns.end());
    break;
  case Insert::After:
    m_insert_after.insert(m_insert_after.end(), insns.begin(), insns.end());
    break;
  case Insert::Replacing:
    always_assert_log(!m_replace.has_value(),
                      "It's not possible to have two Replacing instructions "
                      "for a single anchor.");
    m_replace = std::move(insns);
    break;
  }
}

inline void CFGMutation::insert_before(
    const cfg::InstructionIterator& anchor,
    std::vector<IRInstruction*> instructions) {
  always_assert(!anchor.is_end());
  m_changes[anchor->insn].add_change(ChangeSet::Insert::Before,
                                     std::move(instructions));
}

inline void CFGMutation::insert_after(
    const cfg::InstructionIterator& anchor,
    std::vector<IRInstruction*> instructions) {
  always_assert(!anchor.is_end());
  m_changes[anchor->insn].add_change(ChangeSet::Insert::After,
                                     std::move(instructions));
}

inline void CFGMutation::replace(const cfg::InstructionIterator& anchor,
                                 std::vector<IRInstruction*> instructions) {
  always_assert(!anchor.is_end());
  m_changes[anchor->insn].add_change(ChangeSet::Insert::Replacing,
                                     std::move(instructions));
}

inline void CFGMutation::remove(const cfg::InstructionIterator& anchor) {
  always_assert(!anchor.is_end());
  m_changes[anchor->insn].add_change(ChangeSet::Insert::Replacing, {});
}

inline bool CFGMutation::is_terminal(IROpcode op) {
  return opcode::is_branch(op) || opcode::is_throw(op) ||
         opcode::is_a_return(op);
}

} // namespace cfg
