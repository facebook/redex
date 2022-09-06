/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CFGMutation.h"
#include "IRList.h"
#include "IROpcode.h"
#include "Show.h"
#include "StlUtil.h"
#include "Timer.h"
#include "Trace.h"

namespace cfg {

static AccumulatingTimer s_timer;

double CFGMutation::get_seconds() { return s_timer.get_seconds(); }

void CFGMutation::clear() {
  for (auto& [block, changes] : m_changes) {
    for (auto& [insn, change] : changes) {
      change->dispose();
    }
  }

  m_changes.clear();
}

CFGMutation::ChangeSet* CFGMutation::primary_change_of_move_result(
    cfg::Block* block, ChangeSet& move_result_change) {
  auto& raw_it = move_result_change.get_iterator();
  auto it = block->to_cfg_instruction_iterator(raw_it);
  auto primary_it = m_cfg.primary_instruction_of_move_result(it);
  always_assert(!primary_it.is_end());
  auto primary_insn = primary_it->insn;
  auto primary_block_changes_it = m_changes.find(primary_it.block());
  if (primary_block_changes_it == m_changes.end()) {
    return nullptr;
  }
  auto& primary_block_changes = primary_block_changes_it->second;
  auto primary_changes_it = primary_block_changes.find(primary_insn);
  if (primary_changes_it == primary_block_changes.end()) {
    return nullptr;
  }
  auto primary_change = primary_changes_it->second.get();
  always_assert(primary_change->get_iterator()->insn == primary_insn);
  return primary_change;
}

bool CFGMutation::reduce_block_changes(cfg::Block* block,
                                       Changes& changes,
                                       bool* requires_slow_processing) {
  bool throws_or_returns{false};
  bool may_throw{false};
  std20::erase_if(changes, [&](auto& p) {
    auto& [insn, change] = p;
    if (opcode::is_move_result_any(insn->opcode())) {
      auto primary_change = primary_change_of_move_result(block, *change);
      if (primary_change && primary_change->has_replace()) {
        if (!change->is_simple_empty_replace()) {
          TRACE(CFG, 1,
                "WARNING: Perfoming a non-simple-empty-replace on a "
                "move-result-any whose primary is being replaced should "
                "not be done as it will be ignored:\n%s\n%s",
                SHOW(primary_change->get_iterator()->insn), SHOW(insn));
        }
        change->dispose();
        return true;
      }
    }
    change->scan(&throws_or_returns, &may_throw);
    return false;
  });
  if (changes.empty()) {
    return false;
  }

  if (changes.size() == 1) {
    auto& [insn, change] = *changes.begin();
    *requires_slow_processing =
        opcode::is_move_result_any(insn->opcode()) &&
        change->get_iterator() == block->get_first_insn();
    return true;
  }

  *requires_slow_processing =
      throws_or_returns ||
      (may_throw &&
       m_cfg.get_succ_edge_of_type(block, cfg::EDGE_THROW) != nullptr);
  return true;
}

bool CFGMutation::process_block_changes_slow(cfg::Block* block,
                                             Changes& changes) {
  always_assert(!changes.empty());
  auto ii = ir_list::InstructionIterable(block);
  for (auto it = ii.begin(); it != ii.end();) {
    auto c = changes.find(it->insn);
    if (c == changes.end()) {
      // No change anchored at this instruction.
      ++it;
      continue;
    }
    auto& change = c->second;
    change->apply(m_cfg, block, it);

    // The anchor can be encountered again.  Erase the change to avoid it
    // being applied again.
    changes.erase(c);
    if (changes.empty()) {
      return true;
    }
  }
  return false;
}

void CFGMutation::process_block_changes(cfg::Block* block, Changes& changes) {
  always_assert(!changes.empty());
  for (auto& [insn, change] : changes) {
    auto& raw_it = change->get_iterator();
    always_assert(raw_it->type == MFLOW_OPCODE);
    always_assert(raw_it->insn == insn);
    always_assert(raw_it != block->end());
    ir_list::InstructionIterator it(raw_it, block->end());
    bool iterators_invalidated = change->apply(m_cfg, block, it);
    always_assert(!iterators_invalidated || changes.size() == 1);
  }
  changes.clear();
}

void CFGMutation::flush() {
  auto timer_scope = s_timer.scope();

  if (m_changes.empty()) {
    return;
  }

  // We need to process blocks in (id) order, as some changes might add blocks
  // (with ids), and we want to keep things deterministic.
  struct OrderedChange {
    cfg::Block* block;
    Changes* changes;
    bool slow;
  };
  std::vector<OrderedChange> ordered_changes;
  for (auto& [block, changes] : m_changes) {
    bool slow{false};
    if (reduce_block_changes(block, changes, &slow)) {
      ordered_changes.push_back({block, &changes, slow});
    }
  }
  std::sort(ordered_changes.begin(), ordered_changes.end(),
            [](auto& a, auto& b) { return a.block->id() < b.block->id(); });

  Changes remaining_changes;
  auto next_block_id = m_cfg.get_last_block()->id() + 1;
  for (auto& [block, changes, slow] : ordered_changes) {
    if (!slow) {
      process_block_changes(block, *changes);
      always_assert(changes->empty());
      continue;
    }

    if (process_block_changes_slow(block, *changes)) {
      always_assert(changes->empty());
      continue;
    }
    for (auto& change : *changes) {
      remaining_changes.emplace(std::move(change));
    }
    changes->clear();
  }

  // Insertions might have created new blocks, we process them until we find no
  // more
  for (; !remaining_changes.empty() &&
         next_block_id <= m_cfg.get_last_block()->id();
       next_block_id++) {
    auto block = m_cfg.get_block(next_block_id);
    process_block_changes_slow(block, remaining_changes);
  }

  // The effect of one change can erase the anchor for another.  The changes
  // left behind are the ones whose anchors were removed. They will never be
  // applied so clear them.
  for (auto& [insn, change] : remaining_changes) {
    change->dispose();
  }
  m_changes.clear();
}

CFGMutation::ChangeSet::~ChangeSet() {}

bool CFGMutation::ChangeSet::apply(ControlFlowGraph& cfg,
                                   cfg::Block* block,
                                   ir_list::InstructionIterator& it) {
  always_assert_log(
      !is_terminal(it->insn->opcode()) || m_replace.has_value() ||
          m_insert_after.empty(),
      "Insert after terminal operation without replacing it is prohibited.");
  // Save in case of iterator invalidation.
  bool invalidated = false;
  auto cfg_it = block->to_cfg_instruction_iterator(it);

  // The iterator does not get invalidated when inserting
  // positions as it only walks through IRInstructions.
  for (auto&& pos : m_insert_pos_before) {
    cfg.insert_before(cfg_it, std::move(pos));
  }
  for (auto&& pos : m_insert_pos_after) {
    cfg.insert_after(cfg_it, std::move(pos));
  }

  for (auto&& sb : m_insert_sb_before) {
    cfg.insert_before(cfg_it, std::move(sb));
  }
  for (auto&& sb : m_insert_sb_after) {
    cfg.insert_after(cfg_it, std::move(sb));
  }

  // Sequencing of the many options is really hard. We want to retain the
  // optimized variants, so exclude some combinations.
  redex_assert(!(m_replace.has_value() && (!m_insert_after_var.empty() ||
                                           !m_insert_before_var.empty())));
  redex_assert(!(!m_insert_after.empty() && (!m_insert_after_var.empty() ||
                                             !m_insert_before_var.empty())));
  redex_assert(!(!m_insert_before.empty() && (!m_insert_after_var.empty() ||
                                              !m_insert_before_var.empty())));
  redex_assert(!(!m_insert_after_var.empty() && !m_insert_before_var.empty()));

  if (!m_insert_after_var.empty() || !m_insert_before_var.empty()) {
    if (!m_insert_before_var.empty()) {
      invalidated = cfg.insert_before(cfg_it, m_insert_before_var.begin(),
                                      m_insert_before_var.end());
    } else {
      invalidated = cfg.insert_after(cfg_it, m_insert_after_var.begin(),
                                     m_insert_after_var.end());
    }
  } else if (!m_replace.has_value() && m_insert_after.empty()) {
    invalidated = cfg.insert_before(cfg_it, m_insert_before);
  } else if (!m_replace.has_value() && m_insert_before.empty()) {
    invalidated = cfg.insert_after(cfg_it, m_insert_after);
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

    it++;
    if (it.unwrap() != block->end() && cfg_it->insn->has_move_result_any() &&
        cfg.move_result_of(cfg_it) == block->to_cfg_instruction_iterator(it)) {
      // The iterator is now sitting over the target's move-result, which is
      // also going to be invalidated, increment again.
      it++;
    }
    invalidated = cfg.replace_insns(cfg_it, replacement);
  }

  if (invalidated) {
    // move iterator to the end of the last iterator's block to avoid missing
    // any instructions.
    it = ir_list::InstructionIterable(block).end();
  }

  return invalidated;
}

void CFGMutation::ChangeSet::scan(bool* throws_or_returns, bool* may_throw) {
  auto scan_insn = [&](IRInstruction* insn) {
    auto op = insn->opcode();
    if (opcode::is_throw(op) || opcode::is_a_return(op)) {
      *throws_or_returns = true;
    } else if (opcode::may_throw(op)) {
      *may_throw = true;
    }
  };

  auto scan_insns = [&](const std::vector<IRInstruction*>& insns) {
    for (auto insn : insns) {
      scan_insn(insn);
    }
  };

  auto scan_variants =
      [&](const std::vector<cfg::ControlFlowGraph::InsertVariant>&
              insert_variants) {
        for (auto& insert_variant : insert_variants) {
          if (std::holds_alternative<IRInstruction*>(insert_variant)) {
            scan_insn(std::get<IRInstruction*>(insert_variant));
          }
        }
      };

  scan_insns(m_insert_before);
  if (m_replace) {
    scan_insns(*m_replace);
  }
  scan_insns(m_insert_after);
  scan_variants(m_insert_before_var);
  scan_variants(m_insert_after_var);
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
