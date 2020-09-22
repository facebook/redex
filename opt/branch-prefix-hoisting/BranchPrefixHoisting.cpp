/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BranchPrefixHoisting.h"

#include <algorithm>
#include <iterator>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Util.h"
#include "Walkers.h"

/**
 * This pass eliminates sibling branches that begin with identical instructions,
 * (aka prefix hoisting).
 * example code pattern
 * if (condition) {
 *   insn_1;
 *   insn_2;
 *   insn_3;
 * } else {
 *   insn_1;
 *   insn_2;
 *   insn_4;
 * }
 * will be optimized into
 * insn_1;
 * insn_2;
 * if (condition) {
 *   insn_3;
 * } else {
 *   insn_4;
 * }
 * given that the hoisted instructions doesn't have a side effect on the branch
 * condition.
 *
 * Note: if an instruction gets hoisted may throw, the line numbers in stack
 * trace may be pointing to before the branch.
 */

namespace {
constexpr const char* METRIC_INSTRUCTIONS_HOISTED = "num_instructions_hoisted";
} // namespace

// Record critical registers that will be clobbered by the hoisted insns.
void BranchPrefixHoistingPass::setup_side_effect_on_vregs(
    const IRInstruction& insn, std::unordered_map<reg_t, bool>& vregs) {
  if (!insn.has_dest()) {
    // insn has no destination, can not have a side effect
    return; // we need to return here, otherwise dest() will throw
  }

  auto dest_reg = insn.dest();
  auto it = vregs.find(dest_reg);
  if (it != vregs.end()) {
    it->second = true;
  }
  if (insn.dest_is_wide() && vregs.find(dest_reg + 1) != vregs.end()) {
    vregs.find(dest_reg + 1)->second = true;
  }
}

// takes a list of iterators of the corresponding blocks, find if the
// instruction is common across different blocks at it + iterator_offset
boost::optional<IRInstruction> BranchPrefixHoistingPass::get_next_common_insn(
    std::vector<IRList::iterator> block_iters, // create a copy
    const std::vector<cfg::Block*>& blocks,
    int iterator_offset,
    constant_uses::ConstantUses& constant_uses) {
  const static auto irinsn_hash = [](const IRInstruction& insn) {
    return insn.hash();
  };
  always_assert(block_iters.size() == blocks.size());
  if (block_iters.empty()) {
    // the common of nothing is not defined
    return boost::none;
  }

  for (unsigned i = 0; i < block_iters.size(); i++) {
    for (int j = 0; j < iterator_offset; j++) {
      if (block_iters[i] == blocks[i]->end()) {
        // make sure don't go beyond the end
        return boost::none;
      }
      block_iters[i]++;
    }
  }

  std::unordered_set<IRInstruction, decltype(irinsn_hash)> insns_at_iters(
      3, irinsn_hash);
  // record the (const val) uses and make sure theey are the same
  std::unordered_set<constant_uses::TypeDemand, EnumClassHash>
      const_zero_use_types;

  bool type_analysis_failed = false;

  for (unsigned i = 0; i < block_iters.size(); i++) {
    if (block_iters[i] == blocks[i]->end() ||
        block_iters[i]->type != MFLOW_OPCODE) {
      return boost::none;
    } else {
      auto insn = block_iters[i]->insn;
      auto op = insn->opcode();
      if (op == OPCODE_CONST && insn->has_literal()) {
        // Makesure all the constant uses are of same type before hoisting.
        auto type_demand = constant_uses.get_constant_type_demand(insn);
        if (type_demand == constant_uses::TypeDemand::Error) {
          type_analysis_failed = true;
        } else {
          const_zero_use_types.insert(type_demand);
        }
      }
      insns_at_iters.insert(*insn);
    }
  }
  if (insns_at_iters.size() == 1 && !type_analysis_failed &&
      const_zero_use_types.size() <= 1) {
    return *(insns_at_iters.begin());
  } else {
    return boost::none;
  }
}

bool BranchPrefixHoistingPass::is_block_eligible(cfg::Block* block) {
  // only do the optimization in this pass for if and switches
  auto br = block->branchingness();
  return br == opcode::BRANCH_IF || br == opcode::BRANCH_SWITCH;
}

std::vector<cfg::Block*> get_succ_blocks(cfg::Block* block) {
  std::vector<cfg::Edge*> succ_edges = block->succs();
  std::vector<cfg::Block*> succ_blocks;
  succ_blocks.reserve(succ_edges.size());
  for (auto edge : succ_edges) {
    succ_blocks.push_back(edge->target());
  }
  return succ_blocks;
}

bool BranchPrefixHoistingPass::is_insn_eligible(const IRInstruction& insn) {
  auto op = insn.opcode();
  return !opcode::is_branch(op) && !opcode::is_throw(op);
}

// returns number of hoisted instructions
int BranchPrefixHoistingPass::process_hoisting_for_block(
    cfg::Block* block,
    cfg::ControlFlowGraph& cfg,
    type_inference::TypeInference& type_inference,
    constant_uses::ConstantUses& constant_uses) {

  auto all_preds_are_same = [](const std::vector<cfg::Edge*>& edge_lst) {
    if (edge_lst.size() == 1) {
      return true;
    }
    std::set<cfg::Block*> count;
    for (auto e : edge_lst) {
      count.insert(e->src());
    }
    return count.size() == 1;
  };

  if (!is_block_eligible(block)) {
    return 0;
  }

  // find critical registers that relates to branch taking decisions
  IRList::iterator last_insn_it = block->get_last_insn();
  if (last_insn_it == block->end()) {
    // block is empty
    return 0;
  }
  IRInstruction* last_insn = last_insn_it->insn;
  // critical registers for hoisting and if they are clobbered.
  // they all start as non-clobbered but if any hoisted insn clobbers it, it
  // will be changed to true.
  std::unordered_map<reg_t, bool> crit_regs;
  for (size_t i = 0; i < last_insn->srcs_size(); i++) {
    crit_regs.insert(std::make_pair(last_insn->src(i), false));
    if (last_insn->src_is_wide(i)) {
      crit_regs.insert(std::make_pair(last_insn->src(i) + 1, false));
    }
  }

  std::vector<cfg::Block*> succ_blocks = get_succ_blocks(block);
  // make sure every successor has same predecessor
  for (auto succ_block : succ_blocks) {
    const auto& preds_of_succ_block = succ_block->preds();
    if (!all_preds_are_same(preds_of_succ_block)) {
      // we can only hoist the prefix if the block has only one incoming edge
      return 0;
    }
  }
  auto insns_to_hoist =
      get_insns_to_hoist(succ_blocks, crit_regs, constant_uses);
  if (!insns_to_hoist.empty()) {
    // do the mutation
    return hoist_insns_for_block(block,
                                 last_insn_it,
                                 succ_blocks,
                                 cfg,
                                 insns_to_hoist,
                                 crit_regs,
                                 type_inference);
  }
  return 0;
}

void BranchPrefixHoistingPass::skip_pos_debug(IRList::iterator& it,
                                              const IRList::iterator& end) {
  while (it != end && (it->type == MFLOW_POSITION || it->type == MFLOW_DEBUG)) {
    it++;
  }
}

std::vector<IRInstruction> BranchPrefixHoistingPass::get_insns_to_hoist(
    const std::vector<cfg::Block*>& succ_blocks,
    std::unordered_map<reg_t, bool>& crit_regs,
    constant_uses::ConstantUses& constant_uses) {
  // get iterators that points to the beginning of each block
  std::vector<IRList::iterator> block_iters;
  block_iters.reserve(succ_blocks.size());
  for (auto succ_block : succ_blocks) {
    block_iters.push_back(succ_block->begin());
  }

  // The main while loop here looks for common prefix instructions in
  // succ_blocks. The instruction can be hoist if it doesn't have a side effect
  // on the registers that relate to the branch taking decisions
  bool proceed = true;
  std::vector<IRInstruction> insns_to_hoist;
  while (proceed) {
    // skip pos and debug info and check if at least one block reaches the end
    for (unsigned i = 0; i < block_iters.size(); i++) {
      skip_pos_debug(block_iters[i], succ_blocks[i]->end());
      if (block_iters[i] == succ_blocks[i]->end()) {
        // at least one block is empty, we are done
        return insns_to_hoist;
      }
    }
    boost::optional<IRInstruction> common_insn =
        get_next_common_insn(block_iters, succ_blocks, 0, constant_uses);

    if (common_insn && is_insn_eligible(*common_insn)) {
      IRInstruction only_insn = *common_insn;
      if (only_insn.has_move_result_any()) {
        // need to check, for all succ blocks, the associated move-result must:
        // 1. be in the same block
        // 2. be identical
        // 3. have no side effect on crit_regs
        // otherwise, stop here and do not proceed
        boost::optional<IRInstruction> next_common_insn =
            get_next_common_insn(block_iters, succ_blocks, 1, constant_uses);
        if (!next_common_insn) {
          // next one is not common, or at least one succ block reaches the end
          // give up on this instruction
          proceed = false;
        } else {
          setup_side_effect_on_vregs(*next_common_insn, crit_regs);
        }
      }

      setup_side_effect_on_vregs(only_insn, crit_regs);
      if (proceed) {
        // all conditions satisfied
        insns_to_hoist.push_back(only_insn);
        for (auto& it : block_iters) {
          it++;
        }
      }
    } else {
      // instructions at this position diverges among the successor blocks
      proceed = false;
    }
  }

  return insns_to_hoist;
}

// If critical registers are clobbered by hosited insns, make copy instructions
// and insert them into heap_insn_objs (which will be before hosisted insns). We
// also modify the critical insn to use the copied reg. RegAlloc will fix this
// and will try to remove the copy.
//
bool BranchPrefixHoistingPass::create_move_and_fix_clobbered(
    const IRList::iterator& pos,
    std::vector<IRInstruction*>& heap_insn_objs,
    cfg::Block* block,
    cfg::ControlFlowGraph& cfg,
    const std::unordered_map<reg_t, bool>& crit_regs,
    type_inference::TypeInference& type_inference) {
  std::unordered_map<reg_t, reg_t> reg_map;
  auto it = block->to_cfg_instruction_iterator(pos);
  auto cond_insn = it->insn;
  auto& type_envs = type_inference.get_type_environments();
  auto& env = type_envs.at(cond_insn);

  // Go over the critical regs and make a copy before hoisted insns.
  for (size_t i = 0; i < cond_insn->srcs_size(); i++) {
    auto reg = cond_insn->src(i);
    auto it_reg = crit_regs.find(reg);
    if ((it_reg != crit_regs.end() && it_reg->second) ||
        (cond_insn->src_is_wide(i) &&
         crit_regs.find(reg + 1) != crit_regs.end() &&
         crit_regs.find(reg + 1)->second)) {
      auto type = env.get_type(reg);

      // If type_inference cannot infer type, give-up.
      if (type.is_top() || type.is_bottom()) {
        for (auto insn_ptr : heap_insn_objs) {
          delete insn_ptr;
        }
        return false;
      }
      reg_t tmp_reg;
      if (cond_insn->src_is_wide(i)) {
        tmp_reg = cfg.allocate_wide_temp();
      } else {
        tmp_reg = cfg.allocate_temp();
      }

      // Make a copy.
      IRInstruction* copy_insn;
      if (type.equals(type_inference::TypeDomain(REFERENCE))) {
        copy_insn = new IRInstruction(OPCODE_MOVE_OBJECT);
      } else if (cond_insn->src_is_wide(i)) {
        copy_insn = new IRInstruction(OPCODE_MOVE_WIDE);
      } else {
        copy_insn = new IRInstruction(OPCODE_MOVE);
      }
      reg_map.insert(std::make_pair(reg, tmp_reg));
      copy_insn->set_dest(tmp_reg)->set_src(0, reg);
      heap_insn_objs.push_back(copy_insn);
    }
  }
  // if we have made copy_insns, insert them before hosited insns and change the
  // conditional insn to use the copied (un-clobbered) reg.
  for (size_t i = 0; i < it->insn->srcs_size(); i++) {
    auto reg = it->insn->src(i);
    if (reg_map.find(reg) != reg_map.end()) {
      it->insn->set_src(i, reg_map.find(reg)->second);
    }
  }
  return true;
}

// This function is where the pass mutates the IR
size_t BranchPrefixHoistingPass::hoist_insns_for_block(
    cfg::Block* block,
    const IRList::iterator& pos,
    const std::vector<cfg::Block*>& succ_blocks,
    cfg::ControlFlowGraph& cfg,
    const std::vector<IRInstruction>& insns_to_hoist,
    const std::unordered_map<reg_t, bool>& crit_regs,
    type_inference::TypeInference& type_inference) {

  std::vector<IRInstruction*> heap_insn_objs;
  if (!create_move_and_fix_clobbered(
          pos, heap_insn_objs, block, cfg, crit_regs, type_inference)) {
    return 0;
  }

  // Make a copy of hoisted insns.
  heap_insn_objs.reserve(insns_to_hoist.size());
  for (const IRInstruction& insn : insns_to_hoist) {
    heap_insn_objs.push_back(new IRInstruction(insn));
  }

  // we need to insert instructions to hoist right before the branching
  // instruction, which is the last instruction of the block.
  // `is_block_eligible` makes sure the if or switch instruction is there
  auto it = block->to_cfg_instruction_iterator(pos);
  cfg.insert_before(it, heap_insn_objs);

  std::unordered_set<cfg::Block*> proessed_blocks;
  for (auto succ_block : succ_blocks) {
    if (proessed_blocks.count(succ_block)) {
      continue;
    }
    proessed_blocks.insert(succ_block);
    auto to_remove =
        ir_list::InstructionIterator(succ_block->begin(), succ_block->end());

    for (const auto& insn : insns_to_hoist) {
      if (opcode::is_move_result_any(insn.opcode())) {
        // move result pseudo gets removed along with its associating insn
        continue;
      }
      // verify the insn we want to remove
      always_assert(*(to_remove->insn) == insn);
      succ_block->remove_insn(to_remove);
      to_remove =
          ir_list::InstructionIterator(succ_block->begin(), succ_block->end());
    }
  }
  return insns_to_hoist.size();
}

int BranchPrefixHoistingPass::process_code(IRCode* code, DexMethod* method) {
  code->build_cfg(true);
  auto& cfg = code->cfg();
  type_inference::TypeInference type_inference(cfg);
  type_inference.run(method);
  constant_uses::ConstantUses constant_uses(cfg, method);

  int ret = process_cfg(cfg, type_inference, constant_uses);
  code->clear_cfg();
  return ret;
}

int BranchPrefixHoistingPass::process_cfg(
    cfg::ControlFlowGraph& cfg,
    type_inference::TypeInference& type_inference,
    constant_uses::ConstantUses& constant_uses) {
  int ret_insns_hoisted = 0;
  bool performed_transformation = false;
  do {
    performed_transformation = false;
    const std::vector<cfg::Block*>& blocks =
        graph::postorder_sort<cfg::GraphInterface>(cfg);
    // iterate from the back, may get to the optimal state quicker
    for (auto block : blocks) {
      // when we are processing hoist for one block, other blocks may be changed
      int n_insn_hoisted =
          process_hoisting_for_block(block, cfg, type_inference, constant_uses);
      if (n_insn_hoisted) {
        performed_transformation = true;
        ret_insns_hoisted += n_insn_hoisted;
        break;
      }
    }
  } while (performed_transformation);
  return ret_insns_hoisted;
}

void BranchPrefixHoistingPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& /* unused */,
                                        PassManager& mgr) {
  auto scope = build_class_scope(stores);

  int total_insns_hoisted =
      walk::parallel::methods<int>(scope, [](DexMethod* method) -> int {
        const auto code = method->get_code();
        if (!code) {
          return 0;
        }

        int insns_hoisted =
            BranchPrefixHoistingPass::process_code(code, method);
        if (insns_hoisted) {
          TRACE(BPH, 3,
                "[branch prefix hoisting] Moved %u insns in method {%s}",
                insns_hoisted, SHOW(method));
        }
        return insns_hoisted;
      });

  mgr.incr_metric(METRIC_INSTRUCTIONS_HOISTED, total_insns_hoisted);
}

static BranchPrefixHoistingPass s_pass;
