/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RegAlloc.h"

#include <set>
#include <boost/dynamic_bitset.hpp>

#include "Transform.h"

using LiveSet = boost::dynamic_bitset<>;

std::vector<DexInstruction*> get_instruction_vector(
  const std::vector<Block*>& blocks
) {
  std::vector<DexInstruction*> insns;
  uint16_t id = 0;
  for (auto& block : blocks) {
    for (auto& i : *block) {
      if (i.type == MFLOW_OPCODE) {
        i.addr = id++;
        insns.push_back(i.insn);
      }
    }
  }
  return insns;
}

static bool candidate(DexMethod* m) {
  auto code = m->get_code();
  if (!code) {
    return false;
  }
  auto const& insts = code->get_instructions();
  for (auto const& inst : insts) {
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
  TRACE(REG, 5, "Allocating: %s\n", SHOW(m));
  auto transform =
    MethodTransform::get_method_transform(m, true /* want_cfg */);
  auto& cfg = transform->cfg();
  auto blocks = PostOrderSort(cfg).get();
  auto nregs = m->get_code()->get_registers_size();
  auto ins = m->get_code()->get_ins_size();

  TRACE(REG, 5, "%s\n", SHOW(blocks));

  auto opcode_vector = get_instruction_vector(blocks);
  std::vector<LiveSet> liveness(opcode_vector.size(), LiveSet(nregs));
  std::vector<LiveSet> block_liveness(blocks.size(), LiveSet(nregs));
  bool changed;
  do {
    changed = false;
    for (auto& block : blocks) {
      auto& bliveness = block_liveness[block->id()];
      auto prev_liveness = bliveness;
      bliveness.reset();
      for (auto& succ : block->succs()) {
        bliveness |= block_liveness[succ->id()];
      }
      auto livein = bliveness;
      for (auto it = block->rbegin(); it != block->rend(); ++it) {
        if (it->type != MFLOW_OPCODE) {
          continue;
        }
        auto inst = it->insn;
        auto& iliveness = liveness[it->addr];
        iliveness = livein;
        for (size_t i = 0; i < inst->srcs_size(); i++) {
          iliveness.set(inst->src((int) i));
        }
        livein = iliveness;
        if (inst->dests_size()) {
          iliveness.set(inst->dest());
        }
      }
      bliveness = livein;
      if (bliveness != prev_liveness) {
        changed = true;
      }
    }
  } while (changed);

  // Dump the liveness analysis.
  auto DEBUG_ONLY dumpLiveness = [&] {
    for (auto& block : blocks) {
      for (auto& mie : *block) {
        if (mie.type != MFLOW_OPCODE) {
          continue;
        }
        auto& live = liveness[mie.addr];
        TRACE(REG, 5, "%04x:", mie.addr);
        for (size_t i = 0; i < live.size(); i++) {
          if (live.test(i)) {
            TRACE(REG, 5, " %lu", i);
          }
        }
        TRACE(REG, 5, "\n");
      }
    }
    return "";
  };
  TRACE(REG, 5, "%s", dumpLiveness());

  // Use liveness to build a conflict graph.
  std::vector<std::set<uint16_t>> conflicts(nregs);
  for (auto& block : blocks) {
    for (auto& mie : *block) {
      if (mie.type != MFLOW_OPCODE) {
        continue;
      }
      auto& live = liveness[mie.addr];
      // These are probably sparse so this loop isn't the best choice, but it's
      // easy, and I bet the regsets aren't so big anyways.
      for (size_t i = 0; i < live.size(); i++) {
        for (size_t j = i; j < live.size(); j++) {
          if (live.test(i) && live.test(j)) {
            conflicts[i].emplace(j);
            conflicts[j].emplace(i);
          }
        }
      }
    }
  }

  // Dump the conflict graph.
  auto DEBUG_ONLY dumpConflicts = [&] {
    for (size_t i = 0; i < conflicts.size(); ++i) {
      TRACE(REG, 5, "%lu:", i);
      for (auto& DEBUG_ONLY r : conflicts[i]) {
        TRACE(REG, 5, " %d", r);
      }
      TRACE(REG, 5, "\n");
    }
    return "";
  };
  TRACE(REG, 5, "%s", dumpConflicts());

  // Re-allocate everything but arguments
  std::unordered_map<uint16_t, uint16_t> reg_map;
  for (size_t i = 0; i < (size_t)(nregs - ins); ++i) {
    boost::dynamic_bitset<> conflicted(nregs);
    for (auto& r : conflicts[i]) {
      auto it = reg_map.find(r);
      if (it != reg_map.end()) {
        conflicted.set(it->second);
      }
    }
    for (size_t j = 0; j < conflicted.size(); ++j) {
      if (!conflicted.test(j)) {
        reg_map[i] = j;
        break;
      }
    }
  }

  // handle the arg registers
  boost::dynamic_bitset<> arg_conflicts(nregs);
  for (size_t i = nregs - ins; i < nregs; ++i) {
    for (auto& r : conflicts[i]) {
      auto it = reg_map.find(r);
      if (it != reg_map.end()) {
        arg_conflicts.set(it->second);
      }
    }
  }
  size_t least_arg = 0;
  for (size_t j = nregs; j > 0; --j) {
    if (arg_conflicts.test(j - 1)) {
      least_arg = j;
      break;
    }
  }
  for (size_t i = 0; i < ins; ++i) {
    reg_map[nregs - ins + i] = least_arg++;
  }

  // Resize the code item's register set.
  size_t new_regs = 0;
  for (auto& rr : reg_map) {
    if (rr.second > new_regs) {
      new_regs = rr.second;
    }
  }
  m->get_code()->set_registers_size(new_regs + 1);

  // Dump allocation
  auto DEBUG_ONLY dumpAllocation = [&] {
    for (auto& DEBUG_ONLY rr : reg_map) {
      TRACE(REG, 5, "%hu -> %hu\n", rr.first, rr.second);
    }
    TRACE(REG, 5, "\n");
    return "";
  };
  TRACE(REG, 5, "%s", dumpAllocation());

  for (auto& block : blocks) {
    for (auto& item : *block) {
      if (item.type != MFLOW_OPCODE) {
        continue;
      }
      auto insn = item.insn;
      if (insn->dests_size()) {
        insn->set_dest(reg_map[insn->dest()]);
      }
      for (size_t i = 0; i < insn->srcs_size(); i++) {
        insn->set_src((int) i, reg_map[insn->src((int) i)]);
      }
    }
  }
}
