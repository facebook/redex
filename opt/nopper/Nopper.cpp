/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Nopper.h"

#include "CFGMutation.h"
#include "StlUtil.h"

namespace {
bool can_insert_before(IROpcode opcode) {
  return !opcode::is_a_load_param(opcode) &&
         !opcode::is_move_result_any(opcode) &&
         !opcode::is_move_exception(opcode);
}
}; // namespace

namespace nopper_impl {

std::vector<cfg::Block*> get_noppable_blocks(cfg::ControlFlowGraph& cfg) {
  auto blocks = cfg.blocks();
  std20::erase_if(blocks, [](auto* block) {
    auto ii = InstructionIterable(block);
    auto it = ii.begin();
    while (it != ii.end() && !can_insert_before(it->insn->opcode())) {
      it++;
    }
    return it == ii.end();
  });
  return blocks;
}

size_t insert_nops(cfg::ControlFlowGraph& cfg,
                   const std::unordered_set<cfg::Block*>& blocks) {
  cfg::CFGMutation mutation(cfg);
  size_t nops_inserted = 0;
  for (auto* block : cfg.blocks()) {
    if (!blocks.count(block)) {
      continue;
    }
    auto ii = InstructionIterable(block);
    auto it = ii.begin();
    while (it != ii.end() && !can_insert_before(it->insn->opcode())) {
      it++;
    }
    always_assert(it != ii.end());
    const auto& cfg_it = block->to_cfg_instruction_iterator(it);
    std::vector<IRInstruction*> insns;
    insns.push_back(new IRInstruction(OPCODE_NOP));
    mutation.insert_before(cfg_it, std::move(insns));
    nops_inserted++;
  }
  mutation.flush();
  return nops_inserted;
}

} // namespace nopper_impl
