/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Transform.h"

#include <stack>

#include "ControlFlow.h"

namespace {

using RegMap = transform::RegMap;

void remap_debug(DexDebugInstruction& dbgop, const RegMap& reg_map) {
  switch (dbgop.opcode()) {
  case DBG_START_LOCAL:
  case DBG_START_LOCAL_EXTENDED:
  case DBG_END_LOCAL:
  case DBG_RESTART_LOCAL: {
    auto it = reg_map.find(dbgop.uvalue());
    if (it == reg_map.end()) return;
    dbgop.set_uvalue(it->second);
    break;
  }
  default:
    break;
  }
}

void remap_dest(IRInstruction* inst, const RegMap& reg_map) {
  if (!inst->dests_size()) return;
  auto it = reg_map.find(inst->dest());
  if (it == reg_map.end()) return;
  inst->set_dest(it->second);
}

void remap_srcs(IRInstruction* inst, const RegMap& reg_map) {
  for (unsigned i = 0; i < inst->srcs_size(); i++) {
    auto it = reg_map.find(inst->src(i));
    if (it == reg_map.end()) continue;
    inst->set_src(i, it->second);
  }
}

} // anonymous namespace

namespace transform {

void remap_registers(IRInstruction* insn, const RegMap& reg_map) {
  remap_dest(insn, reg_map);
  remap_srcs(insn, reg_map);
}

void remap_registers(MethodItemEntry& mei, const RegMap& reg_map) {
  switch (mei.type) {
  case MFLOW_OPCODE:
    remap_registers(mei.insn, reg_map);
    break;
  case MFLOW_DEBUG:
    remap_debug(*mei.dbgop, reg_map);
    break;
  default:
    break;
  }
}


void remap_registers(IRCode* code, const RegMap& reg_map) {
  for (auto& mei : *code) {
    remap_registers(mei, reg_map);
  }
}

static size_t remove_block(IRCode* code, cfg::Block* b) {
  size_t insns_removed{0};
  for (auto& mei : InstructionIterable(b)) {
    code->remove_opcode(mei.insn);
    ++insns_removed;
  }
  return insns_removed;
}

/**
 * TODO: The method is deprecated since it doesn't delete the edges associated
 * with the deleted blocks, use ControlFlowGraph::remove_unreachable_blocks()
 * instead.
 */
size_t remove_unreachable_blocks(IRCode* code) {
  auto& cfg = code->cfg();
  const auto& blocks = cfg.blocks();
  size_t insns_removed{0};

  // remove unreachable blocks
  const auto& visited = cfg.visit();
  for (size_t i = 1; i < blocks.size(); ++i) {
    auto& b = blocks.at(i);
    if (visited.test(b->id())) {
      continue;
    }
    // Remove all successor edges. Note that we don't need to try and remove
    // predecessors since by definition, unreachable blocks have no preds
    cfg.delete_succ_edges(b);
    insns_removed += remove_block(code, b);
  }

  return insns_removed;
}

MethodItemEntry* find_active_catch(IRCode* code, IRList::iterator pos) {
  while (++pos != code->end() && pos->type != MFLOW_TRY)
    ;
  return pos != code->end() && pos->tentry->type == TRY_END
             ? pos->tentry->catch_start
             : nullptr;
}

} // namespace transform
