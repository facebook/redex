/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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

void visit(cfg::Block* start, std::unordered_set<cfg::Block*>& visited) {
  std::stack<cfg::Block*> to_visit;
  to_visit.push(start);
  while (!to_visit.empty()) {
    cfg::Block* b = to_visit.top();
    to_visit.pop();

    if (visited.find(b) != visited.end()) {
      continue;
    }
    visited.emplace(b);

    for (auto& s : b->succs()) {
      to_visit.push(s->target());
    }
  }
}

size_t remove_unreachable_blocks(IRCode* code) {
  auto& cfg = code->cfg();
  const auto& blocks = cfg.blocks();
  size_t insns_removed{0};

  // remove unreachable blocks
  std::unordered_set<cfg::Block*> visited;
  visit(blocks.at(0), visited);
  for (size_t i = 1; i < blocks.size(); ++i) {
    auto& b = blocks.at(i);
    if (visited.find(b) != visited.end()) {
      continue;
    }
    // Remove all successor edges. Note that we don't need to try and remove
    // predecessors since by definition, unreachable blocks have no preds
    cfg.remove_succ_edges(b);
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

// TODO: move to CFG
// delete old_block and reroute its predecessors to new_block
//
// if new_block is null, just delete old_block and don't reroute
void replace_block(IRCode* code, cfg::Block* old_block, cfg::Block* new_block) {
  const cfg::ControlFlowGraph& cfg = code->cfg();
  std::vector<MethodItemEntry*> will_move;
  if (new_block != nullptr) {
    // make a copy of the targets we're going to move
    for (MethodItemEntry& mie : *old_block) {
      if (mie.type == MFLOW_TARGET) {
        will_move.push_back(new MethodItemEntry(mie.target));
      }
    }
  }

  // delete old_block
  for (auto it = old_block->begin(); it != old_block->end(); it++) {
    switch (it->type) {
    case MFLOW_OPCODE:
      code->remove_opcode(it);
      break;
    case MFLOW_TARGET:
      it->type = MFLOW_FALLTHROUGH;
      if (new_block == nullptr) {
        delete it->target;
      } // else, new_block takes ownership of the targets
      break;
    default:
      break;
    }
  }

  if (new_block != nullptr) {
    for (auto mie : will_move) {
      // insert the branch target at the beginning of new_block
      // and make sure `m_begin` and `m_end`s point to the right places
      cfg::Block* before = cfg.find_block_that_ends_here(new_block->begin());
      new_block->m_begin = code->insert_before(new_block->begin(), *mie);
      if (before != nullptr) {
        before->m_end = new_block->m_begin;
      }
    }
  }
}

} // namespace transform
