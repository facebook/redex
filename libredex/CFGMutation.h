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
class CFGMutation {
 public:
  enum class Insert { Before, After, Replacing };

  /// Create a new mutation to apply to \p cfg.
  CFGMutation(ControlFlowGraph& cfg);

  /// Any changes remaining in the buffer will be flushed when the mutation is
  /// destroyed.
  ~CFGMutation();

  /// CFGMutation is not copyable
  CFGMutation(const CFGMutation&) = delete;
  CFGMutation& operator=(const CFGMutation&) = delete;

  /// CFGMutation is move-constructable
  CFGMutation(CFGMutation&&) = default;

  /// CFGMutation is not move-assignable.
  CFGMutation& operator=(CFGMutation&&) = delete;

  /// Add a new change to this mutation.
  ///
  /// \p where indicates where to add the  \p instructions, relative to the
  ///     anchor.  \c Before means preserve the anchor instruction and add the
  ///     instructions before it.  \c After means preserve the anchor
  ///     instruction and add the instructions after it.  \c Replacing behaves
  ///     like either \c Before or \c After followed by removing the anchor
  ///     instruction.
  /// \p anchor is the instruction that the change is made relative to.  If at
  ///     the time the change is applied, the anchor does not exist, the change
  ///     will be ignored.
  /// \p instructions are the instructions that are inserted as part of the
  ///     change.  This can be an empty list.
  ///
  /// \pre The anchor iterator must be dereferenceable (i.e. not \c end() ).
  /// \pre This mutation must not have any other change associated with \p
  ///     anchor.
  void add_change(Insert where,
                  const cfg::InstructionIterator& anchor,
                  std::vector<IRInstruction*> instructions);

  /// Apply all the changes that have been added since the last flush (or since
  /// the mutation was created).  Changes are applied in the order they are
  /// added to the mutation.
  void flush();

 private:
  /// A memento of a change we wish to make to the CFG.
  class ChangeSet {
   public:
    ChangeSet(Insert where, std::vector<IRInstruction*> instructions);

    /// Apply this change on the control flow graph \p cfg, using \p it as the
    /// anchoring instruction. Moves \p it if the change invalidates the anchor.
    ///
    ///  - The iterator is guaranteed not to be moved past the first instruction
    ///    after the anchor's initial position.
    ///  - Note the iterator may not be moved at all, even if the change is
    ///    applied.
    void apply(ControlFlowGraph& cfg, InstructionIterator& it);

   private:
    Insert m_where;
    std::vector<IRInstruction*> m_instructions;
  };

  cfg::ControlFlowGraph& m_cfg;
  std::unordered_map<IRInstruction*, ChangeSet> m_changes;
};

inline CFGMutation::CFGMutation(cfg::ControlFlowGraph& cfg) : m_cfg(cfg) {}

inline CFGMutation::~CFGMutation() { flush(); }

inline void CFGMutation::add_change(Insert where,
                                    const cfg::InstructionIterator& anchor,
                                    std::vector<IRInstruction*> instructions) {
  always_assert(!anchor.is_end());
  auto p = m_changes.emplace(anchor->insn,
                             ChangeSet(where, std::move(instructions)));
  always_assert_log(
      p.second, "Conflicting change for anchor: %s.", SHOW(anchor->insn));
}

inline CFGMutation::ChangeSet::ChangeSet(
    Insert where, std::vector<IRInstruction*> instructions)
    : m_where(where), m_instructions(std::move(instructions)) {}

} // namespace cfg
