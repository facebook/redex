/**
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
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
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

bool BranchPrefixHoistingPass::has_side_effect_on_vregs(
    const IRInstruction& insn, const std::unordered_set<uint16_t>& vregs) {
  if (!insn.dests_size()) {
    // insn has no destination, can not have a side effect
    return false; // we need to return here, otherwise dest() will throw
  }

  auto dest_reg = insn.dest();
  if (insn.dest_is_wide()) {
    return (vregs.find(dest_reg) != vregs.end()) ||
           (vregs.find(dest_reg + 1) != vregs.end());
  }
  return vregs.find(dest_reg) != vregs.end();
}

// takes a list of iterators of the corresponding blocks, find if the
// instruction is common across different blocks at it + iterator_offset
boost::optional<IRInstruction> BranchPrefixHoistingPass::get_next_common_insn(
    std::vector<IRList::iterator> block_iters, // create a copy
    const std::vector<cfg::Block*>& blocks,
    int iterator_offset) {
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
  for (unsigned i = 0; i < block_iters.size(); i++) {
    if (block_iters[i] == blocks[i]->end() ||
        block_iters[i]->type != MFLOW_OPCODE) {
      return boost::none;
    } else {
      insns_at_iters.insert(*(block_iters[i]->insn));
    }
  }
  if (insns_at_iters.size() == 1) {
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
  if (op == OPCODE_CONST && insn.has_literal() && insn.get_literal() == 0) {
    // (const v 0) can either be moving number 0 or null pointer to a register
    // we need to exclude this instruction because it causes IRTypeChecker
    // to fail
    return false;
  }
  return !is_branch(op) && !is_throw(op) && !is_return(op);
}

// returns number of hoisted instructions
int BranchPrefixHoistingPass::process_hoisting_for_block(
    cfg::Block* block, cfg::ControlFlowGraph& cfg) {

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
  std::unordered_set<uint16_t> crit_regs(last_insn->srcs().begin(),
                                         last_insn->srcs().end());

  std::vector<cfg::Block*> succ_blocks = get_succ_blocks(block);

  // make sure every successor has only one predecessor
  for (auto succ_block : succ_blocks) {
    const auto& preds_of_succ_block = succ_block->preds();
    if (preds_of_succ_block.size() != 1) {
      // we can only hoist the prefix if the block has only one incoming edge
      return 0;
    }
  }

  auto insns_to_hoist = get_insns_to_hoist(succ_blocks, crit_regs);

  if (!insns_to_hoist.empty()) {
    // do the mutation
    hoist_insns_for_block(
        block, last_insn_it, succ_blocks, cfg, insns_to_hoist);
    return insns_to_hoist.size();
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
    const std::unordered_set<uint16_t>& crit_regs) {
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
        get_next_common_insn(block_iters, succ_blocks, 0);

    if (common_insn && is_insn_eligible(*common_insn)) {
      IRInstruction only_insn = *common_insn;
      if (only_insn.has_move_result()) {
        // need to check, for all succ blocks, the associated move-result must:
        // 1. be in the same block
        // 2. be identical
        // 3. have no side effect on crit_regs
        // otherwise, stop here and do not proceed
        boost::optional<IRInstruction> next_common_insn =
            get_next_common_insn(block_iters, succ_blocks, 1);
        if (!next_common_insn) {
          // next one is not common, or at least one succ block reaches the end
          // give up on this instruction
          proceed = false;
        } else if (has_side_effect_on_vregs(*next_common_insn, crit_regs)) {
          proceed = false;
        }
      }

      if (has_side_effect_on_vregs(only_insn, crit_regs)) {
        // insn can affect the branch taking decision
        proceed = false;
      }
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

// This function is where the pass mutates the IR
void BranchPrefixHoistingPass::hoist_insns_for_block(
    cfg::Block* block,
    const IRList::iterator& pos,
    const std::vector<cfg::Block*>& succ_blocks,
    cfg::ControlFlowGraph& cfg,
    const std::vector<IRInstruction>& insns_to_hoist) {

  std::vector<IRInstruction*> heap_insn_objs;
  heap_insn_objs.reserve(insns_to_hoist.size());
  for (const IRInstruction& insn : insns_to_hoist) {
    heap_insn_objs.push_back(new IRInstruction(insn));
  }

  // we need to insert instructions to hoist right before the branching
  // instruction, which is the last instruction of the block.
  // `is_block_eligible` makes sure the if or switch instruction is there
  auto it = block->to_cfg_instruction_iterator(pos);
  cfg.insert_before(it, heap_insn_objs);

  for (auto succ_block : succ_blocks) {
    auto to_remove =
        ir_list::InstructionIterator(succ_block->begin(), succ_block->end());

    for (auto insn : insns_to_hoist) {
      if (opcode::is_move_result_pseudo(insn.opcode())) {
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
}

int BranchPrefixHoistingPass::process_code(IRCode* code) {
  code->build_cfg(true);
  auto& cfg = code->cfg();
  int ret = process_cfg(cfg);
  code->clear_cfg();
  return ret;
}

int BranchPrefixHoistingPass::process_cfg(cfg::ControlFlowGraph& cfg) {
  int ret_insns_hoisted = 0;
  bool performed_transformation = false;
  do {
    performed_transformation = false;
    const std::vector<cfg::Block*>& blocks = cfg::postorder_sort(cfg.blocks());
    // iterate from the back, may get to the optimal state quicker
    for (auto block : blocks) {
      // when we are processing hoist for one block, other blocks may be changed
      int n_insn_hoisted = process_hoisting_for_block(block, cfg);
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

  int total_insns_hoisted = walk::parallel::reduce_methods<int>(
      scope,
      [](DexMethod* method) -> int {
        const auto code = method->get_code();
        if (!code) {
          return 0;
        }

        int insns_hoisted = BranchPrefixHoistingPass::process_code(code);
        if (insns_hoisted) {
          TRACE(BPH, 3,
                "[branch prefix hoisting] Moved %u insns in method {%s}\n",
                insns_hoisted, SHOW(method));
        }
        return insns_hoisted;
      },
      [](int a, int b) { return a + b; });

  mgr.incr_metric(METRIC_INSTRUCTIONS_HOISTED, total_insns_hoisted);
}

static BranchPrefixHoistingPass s_pass;
