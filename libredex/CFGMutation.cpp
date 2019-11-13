/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CFGMutation.h"

namespace cfg {

void CFGMutation::flush() {
  auto ii = InstructionIterable(m_cfg);
  for (auto it = ii.begin(); !it.is_end();) {
    auto c = m_changes.find(it->insn);
    if (c == m_changes.end()) {
      // No change anchored at this instruction.
      ++it;
      continue;
    }

    auto& change = c->second;
    change.apply(m_cfg, it);

    // The anchor can be encountered again.  Erase the change to avoid it being
    // applied again.
    m_changes.erase(c);
  }

  // The effect of one change can erase the anchor for another.  The changes
  // left behind are the ones whose anchors were removed. They will never be
  // applied so clear them.
  m_changes.clear();
}

void CFGMutation::ChangeSet::apply(ControlFlowGraph& cfg,
                                   InstructionIterator& it) {
  // Save in case of iterator invalidation.
  Block* b = it.block();

  bool invalidated = false;
  switch (m_where) {
  case CFGMutation::Insert::Before:
    invalidated = cfg.insert_before(it, m_instructions);
    break;
  case CFGMutation::Insert::After:
    invalidated = cfg.insert_after(it, m_instructions);
    break;
  case CFGMutation::Insert::Replacing: {
    // The target will be invalidated by the replacement, increment the iterator
    // in preparation.
    auto target = it++;

    if (target->insn->has_move_result_any() && !it.is_end() &&
        cfg.move_result_of(target) == it) {
      // The iterator is now sitting over the target's move-result, which is
      // also going to be invalidated, increment again.
      ++it;
    }

    invalidated = cfg.replace_insns(target, m_instructions);
    break;
  }
  }

  if (invalidated) {
    // move iterator to the end of the last iterator's block to avoid missing
    // any instructions.
    it = b->to_cfg_instruction_iterator(b->end());
  }
}

} // namespace cfg
