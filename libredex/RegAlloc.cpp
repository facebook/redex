/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include <algorithm>
#include <deque>

#include "Dataflow.h"
#include "IRInstruction.h"
#include "RegAlloc.h"
#include "Transform.h"

//////////////////////////////////////////////////////////////////////////////

bool Liveness::operator==(const Liveness& that) const {
  return m_reg_set == that.m_reg_set;
}

void Liveness::enlarge(uint16_t ins_size, uint16_t newregs) {
  if (m_reg_set.size() < newregs) {
    auto oldregs = m_reg_set.size();
    m_reg_set.resize(newregs);
    for (uint16_t i = 0; i < ins_size; ++i) {
      m_reg_set[newregs - 1 - i] = m_reg_set[oldregs - 1 - i];
      m_reg_set[oldregs - 1 - i] = false;
    }
  }
}

void Liveness::trans(const IRInstruction* inst, Liveness* liveness) {
  if (inst->dests_size()) {
    liveness->m_reg_set.reset(inst->dest());
    if (inst->dest_is_wide()) {
      liveness->m_reg_set.reset(inst->dest() + 1);
    }
  }
  for (size_t i = 0; i < inst->srcs_size(); i++) {
    liveness->m_reg_set.set(inst->src((int)i));
    if (inst->src_is_wide((int)i)) {
      liveness->m_reg_set.set(inst->src((int)i) + 1);
    }
  }
  if (inst->has_range()) {
    for (size_t i = 0; i < inst->range_size(); i++) {
      liveness->m_reg_set.set(inst->range_base() + i);
    }
  }
}

void Liveness::meet(const Liveness& that) {
  m_reg_set |= that.m_reg_set;
}

std::unique_ptr<LivenessMap> Liveness::analyze(ControlFlowGraph& cfg,
                                               uint16_t nregs) {
  TRACE(REG, 5, "%s\n", SHOW(cfg));
  auto blocks = postorder_sort(cfg.blocks());
  auto liveness = backwards_dataflow<Liveness>(blocks, Liveness(nregs),
      Liveness::trans);

  auto DEBUG_ONLY dump_liveness = [&](const LivenessMap& amap) {
    for (auto& block : cfg.blocks()) {
      for (auto& mie : *block) {
        if (mie.type != MFLOW_OPCODE) {
          continue;
        }
        auto& analysis = amap.at(mie.insn);
        TRACE(REG, 5, "%04x: %s", mie.addr, SHOW(mie.insn));
        TRACE(REG, 5, " [Live registers:%s]\n", SHOW(analysis));
      }
    }
    return "";
  };
  TRACE(REG, 5, "%s", dump_liveness(*liveness));

  return liveness;
}

//////////////////////////////////////////////////////////////////////////////

static bool candidate(DexMethod* m) {
  auto& code = m->get_code();
  if (!code) {
    return false;
  }
  for (auto const& mie : InstructionIterable(code->get_entries())) {
    auto inst = mie.insn;
    switch (inst->opcode()) {
    case OPCODE_MOVE_WIDE:
    case OPCODE_MOVE_WIDE_FROM16:
    case OPCODE_MOVE_WIDE_16:
    case OPCODE_MOVE_RESULT_WIDE:
    case OPCODE_RETURN_WIDE:
    case OPCODE_CONST_WIDE_16:
    case OPCODE_CONST_WIDE_32:
    case OPCODE_CONST_WIDE:
    case OPCODE_CONST_WIDE_HIGH16:
    case OPCODE_AGET_WIDE:
    case OPCODE_APUT_WIDE:
    case OPCODE_IGET_WIDE:
    case OPCODE_IPUT_WIDE:
    case OPCODE_SGET_WIDE:
    case OPCODE_SPUT_WIDE:
    case OPCODE_CMPL_DOUBLE:
    case OPCODE_CMPG_DOUBLE:
    case OPCODE_NEG_DOUBLE:
    case OPCODE_INT_TO_DOUBLE:
    case OPCODE_LONG_TO_DOUBLE:
    case OPCODE_FLOAT_TO_DOUBLE:
    case OPCODE_DOUBLE_TO_INT:
    case OPCODE_DOUBLE_TO_LONG:
    case OPCODE_DOUBLE_TO_FLOAT:
    case OPCODE_ADD_DOUBLE:
    case OPCODE_SUB_DOUBLE:
    case OPCODE_MUL_DOUBLE:
    case OPCODE_DIV_DOUBLE:
    case OPCODE_REM_DOUBLE:
    case OPCODE_ADD_DOUBLE_2ADDR:
    case OPCODE_SUB_DOUBLE_2ADDR:
    case OPCODE_MUL_DOUBLE_2ADDR:
    case OPCODE_DIV_DOUBLE_2ADDR:
    case OPCODE_REM_DOUBLE_2ADDR:
    case OPCODE_CMP_LONG:
    case OPCODE_NEG_LONG:
    case OPCODE_NOT_LONG:
    case OPCODE_INT_TO_LONG:
    case OPCODE_LONG_TO_INT:
    case OPCODE_LONG_TO_FLOAT:
    case OPCODE_FLOAT_TO_LONG:
    case OPCODE_ADD_LONG:
    case OPCODE_SUB_LONG:
    case OPCODE_MUL_LONG:
    case OPCODE_DIV_LONG:
    case OPCODE_REM_LONG:
    case OPCODE_AND_LONG:
    case OPCODE_OR_LONG:
    case OPCODE_XOR_LONG:
    case OPCODE_SHL_LONG:
    case OPCODE_SHR_LONG:
    case OPCODE_USHR_LONG:
    case OPCODE_ADD_LONG_2ADDR:
    case OPCODE_SUB_LONG_2ADDR:
    case OPCODE_MUL_LONG_2ADDR:
    case OPCODE_DIV_LONG_2ADDR:
    case OPCODE_REM_LONG_2ADDR:
    case OPCODE_AND_LONG_2ADDR:
    case OPCODE_OR_LONG_2ADDR:
    case OPCODE_XOR_LONG_2ADDR:
    case OPCODE_SHL_LONG_2ADDR:
    case OPCODE_SHR_LONG_2ADDR:
    case OPCODE_USHR_LONG_2ADDR:
    case OPCODE_FILLED_NEW_ARRAY_RANGE:
    case OPCODE_INVOKE_VIRTUAL_RANGE:
    case OPCODE_INVOKE_SUPER_RANGE:
    case OPCODE_INVOKE_DIRECT_RANGE:
    case OPCODE_INVOKE_STATIC_RANGE:
    case OPCODE_INVOKE_INTERFACE_RANGE:
      return false;
    default:
      break;
    }
  }
  return true;
}

void allocate_registers(DexMethod* m) {
  if (!candidate(m)) {
    return;
  }
  auto transform = m->get_code()->get_entries();
  transform->build_cfg();
  auto& cfg = transform->cfg();
  auto nregs = m->get_code()->get_registers_size();
  auto liveness_map = Liveness::analyze(cfg, nregs);
  auto ins = m->get_code()->get_ins_size();

  // Use liveness to build a conflict graph.
  std::vector<RegSet> conflicts(nregs, RegSet(nregs));
  for (auto& block : cfg.blocks()) {
    for (auto& mie : *block) {
      if (mie.type != MFLOW_OPCODE) {
        continue;
      }
      std::vector<uint16_t> dest_regs;
      if (mie.insn->dests_size()) {
        dest_regs.push_back(mie.insn->dest());
        if (mie.insn->dest_is_wide()) {
          dest_regs.push_back(mie.insn->dest() + 1);
        }
      }
      auto& live_out = liveness_map->at(mie.insn);
      for (auto r : dest_regs) {
        conflicts[r] |= live_out.bits();
        conflicts[r][r] = false;
      }
    }
  }

  // Dump the conflict graph.
  auto DEBUG_ONLY dumpConflicts = [&] {
    for (size_t i = 0; i < conflicts.size(); ++i) {
      TRACE(REG, 5, "%lu:", i);
      for (size_t j = 0; j < conflicts[i].size(); ++j) {
        if (conflicts[i][j]) {
          TRACE(REG, 5, " %d", j);
        }
      }
      TRACE(REG, 5, "\n");
    }
    return "";
  };
  TRACE(REG, 5, "%s", dumpConflicts());

  // Re-allocate everything but arguments
  std::unordered_map<uint16_t, uint16_t> reg_map;
  size_t new_regs = 0;
  for (size_t i = 0; i < (size_t)(nregs - ins); ++i) {
    RegSet conflicted(new_regs);
    for (size_t j = 0; j < conflicts[i].size(); ++j) {
      if (!conflicts[i][j] && !conflicts[j][i]) {
        continue;
      }
      auto it = reg_map.find(j);
      if (it != reg_map.end()) {
        conflicted.set(it->second);
      }
    }
    RegSet available = ~conflicted;
    auto first_avail = available.find_first();
    if (first_avail == RegSet::npos) {
      first_avail = new_regs;
    }
    reg_map[i] = first_avail;
    new_regs = std::max(new_regs, first_avail + 1);
  }

  // handle the arg registers
  RegSet arg_conflicts(new_regs);
  for (size_t i = nregs - ins; i < nregs; ++i) {
    for (size_t j = 0; j < conflicts[i].size(); ++j) {
      if (!conflicts[i][j] && !conflicts[j][i]) {
        continue;
      }
      auto it = reg_map.find(j);
      if (it != reg_map.end()) {
        arg_conflicts.set(it->second);
      }
    }
  }
  int32_t least_arg = 0;
  for (size_t j = new_regs; j > 0; --j) {
    if (arg_conflicts.test(j - 1)) {
      least_arg = j;
      break;
    }
  }
  least_arg = std::max(least_arg, (int32_t)new_regs - ins);
  for (size_t i = 0; i < ins; ++i) {
    reg_map[nregs - ins + i] = least_arg++;
    new_regs = least_arg;
  }

  if (new_regs > 16) {
    return;
  }

  // Resize the code item's register set.
  m->get_code()->set_registers_size(new_regs);

  // Dump allocation
  auto DEBUG_ONLY dumpAllocation = [&] {
    for (auto& DEBUG_ONLY rr : reg_map) {
      TRACE(REG, 5, "%hu -> %hu\n", rr.first, rr.second);
    }
    TRACE(REG, 5, "\n");
    return "";
  };
  TRACE(REG, 5, "%s", dumpAllocation());

  for (auto& block : cfg.blocks()) {
    for (auto& item : *block) {
      if (item.type != MFLOW_OPCODE) {
        continue;
      }
      auto insn = item.insn;
      for (size_t i = 0; i < insn->srcs_size(); i++) {
        insn->set_src((int) i, reg_map[insn->src((int) i)]);
      }
      if (insn->dests_size()) {
        insn->set_dest(reg_map[insn->dest()]);
      }
    }
  }
}
