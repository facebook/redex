/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MonitorCount.h"

#include <algorithm>

#include "IRCode.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

bool has_catch_all(const cfg::ControlFlowGraph& cfg, const cfg::Block* block) {
  const auto& throw_edges = cfg.get_succ_edges_of_type(block, cfg::EDGE_THROW);
  return std::any_of(
      throw_edges.begin(), throw_edges.end(), [](const auto* edge) {
        return edge->throw_info()->catch_type == nullptr;
      });
}

} // namespace

namespace monitor_count {

void mark_sketchy_methods_with_no_optimize(const Scope& scope) {
  walk::parallel::code(scope, [](DexMethod* method, IRCode& code) {
    code.build_cfg();
    auto bad_insn = find_synchronized_throw_outside_catch_all(code);
    if (bad_insn != nullptr) {
      TRACE(MONITOR, 3,
            "%s has a synchronized may-throw outside a catch-all: %s\n",
            SHOW(method), SHOW(bad_insn));
      method->rstate.set_no_optimizations();
      method->rstate.set_dont_inline();
    }
    code.clear_cfg();
  });
}

IRInstruction* find_synchronized_throw_outside_catch_all(const IRCode& code) {
  auto& cfg = code.cfg();
  Analyzer analyzer(cfg);
  analyzer.run(MonitorCountDomain(0));

  for (auto* block : cfg.blocks()) {
    auto count = analyzer.get_entry_state_at(block);
    if (!count.is_value()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      analyzer.analyze_instruction(insn, &count);

      auto op = insn->opcode();
      bool can_throw = op == OPCODE_THROW || opcode::may_throw(op);
      auto monitor_count = *count.get_constant();
      if (can_throw && monitor_count != 0 && !has_catch_all(cfg, block)) {
        if (op == OPCODE_MONITOR_ENTER && monitor_count == 1) {
          continue;
        }
        return insn;
      }
    }
  }

  return nullptr;
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

} // namespace monitor_count
