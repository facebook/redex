/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CFGMutation.h"

namespace cfg {

void CFGMutation::clear() {
  for (auto& change : m_changes) {
    change.second.dispose();
  }

  m_changes.clear();
}

void CFGMutation::flush() {
  auto ii = InstructionIterable(m_cfg);
  for (auto it = ii.begin(); !m_changes.empty() && !it.is_end();) {
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
  clear();
}

void CFGMutation::ChangeSet::apply(ControlFlowGraph& cfg,
                                   InstructionIterator& it) const {
  always_assert_log(
      !is_terminal(it->insn->opcode()) || m_replace.has_value() ||
          m_insert_after.empty(),
      "Insert after terminal operation without replacing it is prohibited.");

  // Save in case of iterator invalidation.
  Block* b = it.block();
  bool invalidated = false;

  if (!m_replace.has_value() && m_insert_after.empty()) {
    invalidated = cfg.insert_before(it, m_insert_before);
  } else if (!m_replace.has_value() && m_insert_before.empty()) {
    invalidated = cfg.insert_after(it, m_insert_after);
  } else {
    std::vector<IRInstruction*> replacement;
    if (!m_insert_before.empty()) {
      std::copy(m_insert_before.begin(),
                m_insert_before.end(),
                std::back_inserter(replacement));
    }
    if (m_replace.has_value()) {
      std::copy(m_replace.get().begin(),
                m_replace.get().end(),
                std::back_inserter(replacement));
    } else {
      // Copying to avoid problem, replacing insn B with A-B-C
      auto* insn_copy = new IRInstruction(*it->insn);
      replacement.emplace_back(insn_copy);
    }

    if (!m_insert_after.empty()) {
      std::copy(m_insert_after.begin(),
                m_insert_after.end(),
                std::back_inserter(replacement));
    }

    auto target = it++;
    if (target->insn->has_move_result_any() &&
        cfg.move_result_of(target) == it && !it.is_end()) {
      // The iterator is now sitting over the target's move-result, which is
      // also going to be invalidated, increment again.
      ++it;
    }
    invalidated = cfg.replace_insns(target, replacement);
  }

  if (invalidated) {
    // move iterator to the end of the last iterator's block to avoid missing
    // any instructions.
    it = b->to_cfg_instruction_iterator(b->end());
  }
}

namespace {

void dispose_insns(std::vector<IRInstruction*>& insns) {
  for (auto* insn : insns) {
    delete insn;
  }
  insns.clear();
}

} // namespace

void CFGMutation::ChangeSet::dispose() {
  dispose_insns(m_insert_before);
  dispose_insns(m_insert_after);
  if (m_replace) {
    dispose_insns(*m_replace);
  }
}

} // namespace cfg
