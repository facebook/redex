/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BranchPrefixHoisting.h"

#include <algorithm>
#include <boost/optional/optional.hpp>
#include <iterator>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IRList.h"
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
 * We leave debug and position info in the original block. This is required for
 * correctness of the suffix.
 *
 * We hoist source blocks. The reasoning for that is tracking of exceptional
 * flow.
 *
 * Note: if an instruction gets hoisted may throw, the line numbers in stack
 * trace may be pointing to before the branch.
 */

namespace {
constexpr const char* METRIC_INSTRUCTIONS_HOISTED = "num_instructions_hoisted";

// Record critical registers that will be clobbered by the hoisted insns.
void setup_side_effect_on_vregs(const IRInstruction& insn,
                                std::unordered_map<reg_t, bool>& vregs) {
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
boost::optional<IRInstruction> get_next_common_insn(
    const std::vector<IRList::iterator>& block_iters,
    const std::vector<cfg::Block*>& blocks,
    constant_uses::ConstantUses& constant_uses) {
  const static auto irinsn_hash = [](const IRInstruction& insn) {
    return insn.hash();
  };
  always_assert(block_iters.size() == blocks.size());
  if (block_iters.empty()) {
    // the common of nothing is not defined
    return boost::none;
  }

  std::unordered_set<IRInstruction, decltype(irinsn_hash)> insns_at_iters(
      3, irinsn_hash);
  // record the (const val) uses and make sure theey are the same
  std::unordered_set<constant_uses::TypeDemand, EnumClassHash>
      const_zero_use_types;

  bool type_analysis_failed = false;

  for (size_t i = 0; i < block_iters.size(); i++) {
    if (block_iters[i] == blocks[i]->end()) {
      return boost::none;
    }
    if (block_iters[i]->type != MFLOW_OPCODE) {
      return boost::none;
    }

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

  if (insns_at_iters.size() != 1 || type_analysis_failed ||
      const_zero_use_types.size() > 1) {
    return boost::none;
  }

  return *(insns_at_iters.begin());
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

bool is_block_eligible(cfg::Block* block) {
  // only do the optimization in this pass for if and switches
  auto br = block->branchingness();
  return br == opcode::BRANCH_IF || br == opcode::BRANCH_SWITCH;
}

bool is_insn_eligible(const IRInstruction& insn) {
  auto op = insn.opcode();
  return !opcode::is_branch(op) && !opcode::is_throw(op);
}

// Skip over MethodItemEntries that we "handle" in some way:
//  * POSITION, DEBUG: Remain in the block.
//  * SOURCE_BLOCK: Will get hoisted.
// Other types will block hoisting further instructions.
void skip_handled_method_item_entries(IRList::iterator& it,
                                      const IRList::iterator& end) {
  while (it != end && (it->type == MFLOW_POSITION || it->type == MFLOW_DEBUG ||
                       it->type == MFLOW_SOURCE_BLOCK)) {
    it++;
  }
}

std::vector<IRInstruction> get_insns_to_hoist(
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
    enum ItersState { kInconsistent, kAllAtEnd, kAllOngoing };
    auto skip_and_check_end = [&succ_blocks](auto& iters, size_t advance) {
      auto check_one = [advance](auto& it, const auto& end) {
        if (it == end) {
          return false;
        }
        for (size_t j = 0; j != advance; ++j) {
          ++it;
          if (it == end) {
            return false;
          }
        }
        skip_handled_method_item_entries(it, end);
        if (it == end) {
          return false;
        }
        return true;
      };

      size_t at_end = 0;
      for (size_t i = 0; i < iters.size(); i++) {
        if (!check_one(iters[i], succ_blocks[i]->end())) {
          ++at_end;
        }
      }
      return at_end == 0              ? kAllOngoing
             : at_end == iters.size() ? kAllAtEnd
                                      : kInconsistent;
    };

    if (skip_and_check_end(block_iters, /*advance=*/0) != kAllOngoing) {
      TRACE(BPH, 5, "At least one successor is at end");
      return insns_to_hoist;
    }

    boost::optional<IRInstruction> common_insn =
        get_next_common_insn(block_iters, succ_blocks, constant_uses);
    if (common_insn) {
      TRACE(BPH, 5, "Next common instruction: %s", SHOW(*common_insn));
    }

    if (common_insn && is_insn_eligible(*common_insn)) {
      IRInstruction only_insn = *common_insn;
      if (only_insn.has_move_result_any()) {
        // need to check, for all succ blocks, the associated move-result must:
        // 1. be in the same block
        // 2. be identical
        // 3. have no side effect on crit_regs
        // otherwise, stop here and do not proceed
        auto copy = block_iters;
        auto iters_state = skip_and_check_end(copy, /*advance=*/1);
        switch (iters_state) {
        case kInconsistent:
          // TODO: If the existing continuitions are not move-results, and the
          // blocks at end do not have the move-result in their successors, we
          // can hoist, still (as there is no move-result, then).
          TRACE(BPH, 5, "Successors in inconsistent end state.");
          proceed = false;
          break;
        case kAllAtEnd:
          // TODO: If the successors of the successors do not contain
          // move-results, then we can still hoist (as there is no move-result,
          // then).
          TRACE(BPH, 5, "All successors at end.");
          proceed = false;
          break;
        case kAllOngoing: {
          boost::optional<IRInstruction> next_common_insn =
              get_next_common_insn(copy, succ_blocks, constant_uses);
          if (!next_common_insn) {
            TRACE(BPH, 5, "No common successor for move-result-any opcode.");
            proceed = false;
            break;
          }
          // This is OK, but should really only be done for a move-result.
          setup_side_effect_on_vregs(*next_common_insn, crit_regs);
        } break;
        }
      }

      setup_side_effect_on_vregs(only_insn, crit_regs);
      if (proceed) {
        // all conditions satisfied
        insns_to_hoist.push_back(only_insn);
        for (size_t i = 0; i != block_iters.size(); ++i) {
          redex_assert(block_iters[i] != succ_blocks[i]->end());
          redex_assert(block_iters[i]->type == MFLOW_OPCODE);
          redex_assert(*block_iters[i]->insn == only_insn);
          ++block_iters[i];
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
bool create_move_and_fix_clobbered(
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
size_t hoist_insns_for_block(cfg::Block* block,
                             const IRList::iterator& pos,
                             const std::vector<cfg::Block*>& succ_blocks,
                             cfg::ControlFlowGraph& cfg,
                             const std::vector<IRInstruction>& insns_to_hoist,
                             const std::unordered_map<reg_t, bool>& crit_regs,
                             type_inference::TypeInference& type_inference) {
  auto insert_it = block->to_cfg_instruction_iterator(pos);

  {
    std::vector<IRInstruction*> heap_insn_objs;
    if (!create_move_and_fix_clobbered(pos, heap_insn_objs, block, cfg,
                                       crit_regs, type_inference)) {
      return 0;
    }

    // Make a copy of hoisted insns.
    cfg.insert_before(insert_it, heap_insn_objs);
  }

  // Hoist and delete instructions.

  auto succs = [&]() {
    std::unordered_set<cfg::Block*> succ_blocks_set{succ_blocks.begin(),
                                                    succ_blocks.end()};
    std::vector<std::pair<cfg::Block*, IRList::iterator>> ret;
    ret.reserve(succ_blocks_set.size());
    std::transform(succ_blocks_set.begin(), succ_blocks_set.end(),
                   std::back_inserter(ret),
                   [](auto* b) { return std::make_pair(b, b->begin()); });

    // Need to order in some way for stable insertion of source blocks.
    std::sort(ret.begin(), ret.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.first->id() < rhs.first->id();
    });

    return ret;
  }();

  const bool any_throw = std::any_of(
      insns_to_hoist.begin(), insns_to_hoist.end(),
      [](const auto& insn) { return opcode::can_throw(insn.opcode()); });

  DexPosition* last_position = nullptr;
  for (const auto& insn : insns_to_hoist) {
    // Check if any source blocks or positions precede instructions.
    if (!opcode::is_move_result_any(insn.opcode())) {
      for (auto& p : succs) {
        auto* b = p.first;
        auto& it = p.second;
        redex_assert(it != b->end()); // Should have instructions.
        IRList::iterator next;
        for (; it != b->end(); it = next) {
          next = std::next(it);
          if (it->type == MFLOW_OPCODE) {
            break;
          }
          // Leave debug in the block.
          if (it->type == MFLOW_DEBUG) {
            continue;
          }
          // Hoist source blocks and clone positions.
          // TODO: Collapse equivalent source blocks?
          // TODO: Deal with duplication.
          switch (it->type) {
          case MFLOW_SOURCE_BLOCK:
            // The situation is complicated (besides not tracking control flow
            // correctly, being approximative), as inlining may have produced
            // straight code where avoiding that duplication is not obvious. For
            // example:
            //
            // SB1 - NT1 - T1 - SB2 - NT2 | T2 - SB3
            //
            // In this case, it would be best to leave SB2 in the block, as it
            // gives better information than SB3 (or may be necessary to have
            // any SB in the remaining block!).
            //
            // For simplicity, if any instruction to hoist throws, we *copy*
            // *all* source blocks we encounter. This will duplicate every SB,
            // but avoids complicated tracking of what to hoist, clone, or leave
            // alone. Duplication is not an issue for coverage profiling, but
            // for counting.
            // TODO: Revisit.
            //
            // If all hoisted instructions do not throw, just move the
            // instructions. It is safe to do so, as no "additional" control
            // flow is being introduced, such that the SBs in the old block will
            // give the precise information.
            if (any_throw) {
              cfg.insert_before(insert_it,
                                std::make_unique<SourceBlock>(*it->src_block));
            }
            break;
          case MFLOW_POSITION:
            last_position = it->pos.get();
            break;
          default:
            not_reached();
          }
        }
        redex_assert(it != b->end());
      }
    }

    if (opcode::may_throw(insn.opcode()) && last_position) {
      // We clone positions instead of moving, so that we don't move away
      // any initial positions from the sacrificial block. In case of
      // adjacent positions, the cfg will clean up obvious redundancy.
      cfg.insert_before(insert_it,
                        std::make_unique<DexPosition>(*last_position));
    }

    // Insert instruction.
    cfg.insert_before(insert_it, new IRInstruction(insn));

    // Delete instruction from successors.
    if (opcode::is_move_result_any(insn.opcode())) {
      // move result pseudo gets removed along with its associating insn
      continue;
    }

    for (auto& p : succs) {
      auto* b = p.first;
      auto& it = p.second;
      redex_assert(it != b->end());
      redex_assert(it->type == MFLOW_OPCODE);
      redex_assert(*it->insn == insn);

      // Deleting does not have a good API. Gymnastics here.
      boost::optional<IRList::iterator> prev =
          it != b->begin() ? boost::make_optional(std::prev(it)) : boost::none;
      b->remove_insn(it);
      if (prev) {
        it = std::next(*prev);
      } else {
        it = b->begin();
      }
    }
  }

  // Just a sanity check.
  for (const auto& p : succs) {
    const auto& end = p.second;
    for (auto it = p.first->begin(); it != end; ++it) {
      redex_assert(it->type == MFLOW_DEBUG || it->type == MFLOW_POSITION ||
                   it->type == MFLOW_SOURCE_BLOCK);
    }
  }

  return insns_to_hoist.size();
}

// returns number of hoisted instructions
size_t process_hoisting_for_block(cfg::Block* block,
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
    auto hoisted = hoist_insns_for_block(block,
                                         last_insn_it,
                                         succ_blocks,
                                         cfg,
                                         insns_to_hoist,
                                         crit_regs,
                                         type_inference);
    TRACE(
        BPH, 5, "Hoisted %zu/%zu instruction from %s into B%zu", hoisted,
        insns_to_hoist.size(),
        [&]() {
          std::ostringstream oss;
          for (auto& insn : insns_to_hoist) {
            oss << show(insn) << " | ";
          }
          return oss.str();
        }()
            .c_str(),
        block->id());
    return hoisted;
  }
  return 0;
}

} // namespace

size_t BranchPrefixHoistingPass::process_code(IRCode* code, DexMethod* method) {
  code->build_cfg(true);
  auto& cfg = code->cfg();
  TRACE(BPH, 5, "%s", SHOW(cfg));
  type_inference::TypeInference type_inference(cfg);
  type_inference.run(method);
  constant_uses::ConstantUses constant_uses(cfg, method);

  size_t ret = process_cfg(cfg, type_inference, constant_uses);
  code->clear_cfg();
  return ret;
}

size_t BranchPrefixHoistingPass::process_cfg(
    cfg::ControlFlowGraph& cfg,
    type_inference::TypeInference& type_inference,
    constant_uses::ConstantUses& constant_uses) {
  size_t ret_insns_hoisted = 0;
  bool performed_transformation = false;
  do {
    performed_transformation = false;
    const std::vector<cfg::Block*>& blocks =
        graph::postorder_sort<cfg::GraphInterface>(cfg);
    // iterate from the back, may get to the optimal state quicker
    for (auto block : blocks) {
      // when we are processing hoist for one block, other blocks may be changed
      size_t n_insn_hoisted =
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
        TraceContext context{method};

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
