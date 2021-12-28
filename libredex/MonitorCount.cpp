/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MonitorCount.h"

#include <algorithm>

#include "DexClass.h"
#include "IRCode.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

bool has_catch(const cfg::ControlFlowGraph& cfg, const cfg::Block* block) {
  return !!cfg.get_succ_edge_of_type(block, cfg::EDGE_THROW);
}

bool has_catch_all(const cfg::ControlFlowGraph& cfg, const cfg::Block* block) {
  return !!cfg.get_succ_edge_if(block, [](const auto* edge) {
    return edge->type() == cfg::EDGE_THROW &&
           edge->throw_info()->catch_type == nullptr;
  });
}

bool has_try(const IRCode& code) {
  if (code.cfg_built()) {
    auto& cfg = code.cfg();
    for (auto block : cfg.blocks()) {
      if (has_catch(cfg, block)) {
        return true;
      }
    }
    return false;
  }
  for (auto& mie : code) {
    if (mie.type == MFLOW_TRY) {
      return true;
    }
  }
  return false;
}

bool has_try_without_catch_all(const cfg::ControlFlowGraph& cfg) {
  for (auto block : cfg.blocks()) {
    if (has_catch(cfg, block) && !has_catch_all(cfg, block)) {
      return true;
    }
  }
  return false;
}

static bool is_invoke_insn_in_try(const cfg::ControlFlowGraph& cfg,
                                  const IRInstruction* invoke_insn) {
  if (invoke_insn) {
    auto it = cfg.find_insn(const_cast<IRInstruction*>(invoke_insn));
    return !it.is_end() && has_catch(cfg, it.block());
  }
  for (auto block : cfg.blocks()) {
    if (!has_catch(cfg, block)) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      if (opcode::is_an_invoke(mie.insn->opcode())) {
        return true;
      }
    }
  }
  return false;
}

static bool contains_invoke_insn(
    const std::vector<cfg::InstructionIterator>& insns,
    const IRInstruction* invoke_insn) {
  if (invoke_insn) {
    for (auto& it : insns) {
      if (it->insn == invoke_insn) {
        return true;
      }
    }
  } else {
    for (auto& it : insns) {
      if (opcode::is_an_invoke(it->insn->opcode())) {
        return true;
      }
    }
  }
  return false;
}
} // namespace

namespace monitor_count {

std::vector<cfg::Block*> Analyzer::get_monitor_mismatches() {
  std::vector<cfg::Block*> blocks;
  for (auto* block : m_cfg.blocks()) {
    auto count = get_entry_state_at(block);
    if (count.is_bottom()) {
      // Dead block
      continue;
    }
    if (count.is_top()) {
      blocks.push_back(block);
    }
  }
  for (auto* block : m_cfg.return_blocks()) {
    auto count = get_exit_state_at(block);
    if (count.is_value() && *count.get_constant() != 0) {
      blocks.push_back(block);
    }
  }
  return blocks;
}

std::vector<cfg::InstructionIterator> Analyzer::get_sketchy_instructions() {
  std::vector<cfg::InstructionIterator> res;
  for (auto* block : m_cfg.blocks()) {
    auto count = get_entry_state_at(block);
    if (!count.is_value()) {
      // Dead block or monitor mismatch
      continue;
    }
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      auto insn = it->insn;
      analyze_instruction(insn, &count);

      auto op = insn->opcode();
      bool can_throw = op == OPCODE_THROW || opcode::may_throw(op);
      auto monitor_count = *count.get_constant();
      if (can_throw && monitor_count != 0 && !has_catch_all(m_cfg, block)) {
        if (op == OPCODE_MONITOR_ENTER && monitor_count == 1) {
          continue;
        }
        res.push_back(block->to_cfg_instruction_iterator(it));
      }
    }
  }

  return res;
}

MonitorCountDomain Analyzer::analyze_edge(
    const EdgeId& edge, const MonitorCountDomain& exit_state_at_source) const {
  auto env = exit_state_at_source;
  if (!env.is_value() || edge->type() != cfg::EDGE_THROW) {
    return env;
  }
  auto last_insn_it = edge->src()->get_last_insn();
  always_assert(last_insn_it != edge->src()->end());

  // undo counter change in case of failure (throw-edge)
  auto insn = last_insn_it->insn;
  auto op = insn->opcode();
  switch (op) {
  case OPCODE_MONITOR_ENTER:
    return MonitorCountDomain(*env.get_constant() - 1);
  case OPCODE_MONITOR_EXIT:
    // A monitor exit is not actually handled as throwing. See
    // https://cs.android.com/android/platform/superproject/+/android-4.0.4_r2.1:dalvik/vm/analysis/CodeVerify.cpp;l=4146
    //
    // As such, pretend this edge isn't there.
    return MonitorCountDomain::bottom();
  case OPCODE_INVOKE_STATIC: {
    // We have observed that the Kotlin compiler injects invocations to a
    // certain marker in a way that causes imbalanced monitor stack. We choose
    // to ignore that here.
    auto method = insn->get_method()->as_def();
    if (method) {
      const auto& name = method->get_fully_deobfuscated_name();
      if (name == "Lkotlin/jvm/internal/InlineMarker;.finallyStart:(I)V") {
        return MonitorCountDomain::bottom();
      }
    }
    break;
  }
  default:
    break;
  }
  return env;
}

void Analyzer::analyze_instruction(const IRInstruction* insn,
                                   MonitorCountDomain* current) const {
  if (!current->is_value()) {
    return;
  }
  auto op = insn->opcode();
  switch (op) {
  case OPCODE_MONITOR_ENTER:
    *current = MonitorCountDomain(*current->get_constant() + 1);
    break;
  case OPCODE_MONITOR_EXIT:
    *current = MonitorCountDomain(*current->get_constant() - 1);
    break;
  default:
    break;
  }
}

bool cannot_inline_sketchy_code(const IRCode& caller,
                                const IRCode& callee,
                                const IRInstruction* invoke_insn) {
  // All failure conditions depend on that we have some try regions.
  if (!has_try(callee) || !has_try(caller)) {
    return false;
  }

  // We are conservative without cfgs.
  if (!callee.cfg_built() || !caller.cfg_built()) {
    return true;
  }

  // If callee has try regions without catch-alls, we must not inline that at a
  // sketchy call-site.
  if (has_try_without_catch_all(callee.cfg())) {
    auto caller_sketchy_insns =
        Analyzer(caller.cfg()).get_sketchy_instructions();
    if (contains_invoke_insn(caller_sketchy_insns, invoke_insn)) {
      return true;
    }
  }

  // The caller has try regions. Let's make sure we won't inline a sketchy
  // method into a try region.
  auto is_callee_sketchy =
      !Analyzer(callee.cfg()).get_sketchy_instructions().empty();
  if (is_callee_sketchy && is_invoke_insn_in_try(caller.cfg(), invoke_insn)) {
    return true;
  }

  return false;
}

} // namespace monitor_count
