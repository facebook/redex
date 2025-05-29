/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Transform.h"

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
  if (!inst->has_dest()) return;
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
  if (!code->editable_cfg_built()) {
    for (auto& mei : *code) {
      remap_registers(mei, reg_map);
    }
  } else {
    for (auto& mei : cfg::InstructionIterable(code->cfg())) {
      remap_registers(mei, reg_map);
    }
  }
}
void remap_registers(cfg::ControlFlowGraph& cfg, const RegMap& reg_map) {
  for (auto& mei : cfg::InstructionIterable(cfg)) {
    remap_registers(mei, reg_map);
  }
}

MethodItemEntry* find_active_catch(IRCode* code, IRList::iterator pos) {
  while (++pos != code->end() && pos->type != MFLOW_TRY)
    ;
  return pos != code->end() && pos->tentry->type == TRY_END
             ? pos->tentry->catch_start
             : nullptr;
}

} // namespace transform
