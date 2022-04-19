/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Split.h"
#include "Show.h"
#include "Trace.h"

namespace regalloc {

// Calculate potential split costs for each live range. Also store information
// of catch block and move-result for later use.
void calc_split_costs(const LivenessFixpointIterator& fixpoint_iter,
                      IRCode* code,
                      SplitCosts* split_costs) {
  auto& cfg = code->cfg();
  for (cfg::Block* block : cfg.blocks()) {
    LivenessDomain live_out = fixpoint_iter.get_live_out_vars_at(block);
    // Incrementing load number for each death in
    // LiveOut(block) - LiveIn(succs).
    for (auto& succ : block->succs()) {
      LivenessDomain live_in =
          fixpoint_iter.get_live_in_vars_at(succ->target());
      for (auto reg : live_out.elements()) {
        if (!live_in.contains(reg)) {
          split_costs->increase_load(reg);
          // Record how many death on edge occured at certain catch block.
          if (succ->type() == cfg::EDGE_THROW) {
            split_costs->add_catch_block(reg, succ->target());
          } else {
            // Record death on edge to non-catch block;
            split_costs->add_other_block(reg, succ->target());
          }
        }
      }
    }
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      auto insn = it->insn;
      // Add store cost for each define.
      if (insn->has_dest()) {
        split_costs->increase_store(insn->dest());
        // Since move-result must immediately follow invoke-xxx or
        // fill-new-array, so if there is a def of move-result kind, we need to
        // store it and take care of it later to avoid splitting a value s0
        // around another value s1 where s0 is in invoke-xxx and s1 is in
        // move-result.
        if (opcode::is_a_move_result(insn->opcode())) {
          auto prev_insn = std::prev(it.base(), 2);
          while (prev_insn->type != MFLOW_OPCODE) {
            --prev_insn;
          }
          split_costs->add_write_result(insn->dest(), &*prev_insn);
        }
      }
      // Add load cost for each death.
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        auto src = insn->src(i);
        if (!live_out.contains(src)) {
          split_costs->increase_load(src);
        }
      }
      fixpoint_iter.analyze_instruction(it->insn, &live_out);
    }
  }
}

IRInstruction* gen_load_for_split(
    const Graph& ig,
    vreg_t l,
    std::unordered_map<vreg_t, vreg_t>* load_store_reg,
    IRCode* code) {
  auto load_reg_it = load_store_reg->find(l);
  if (load_reg_it == load_store_reg->end()) {
    auto temp = code->cfg().allocate_temp();
    load_store_reg->emplace(l, temp);
    return gen_move(ig.get_node(l).type(), l, temp);
  } else {
    return gen_move(ig.get_node(l).type(), l, load_reg_it->second);
  }
}

IRInstruction* gen_store_for_split(
    const Graph& ig,
    vreg_t l,
    std::unordered_map<vreg_t, vreg_t>* load_store_reg,
    IRCode* code) {
  auto store_reg_it = load_store_reg->find(l);
  if (store_reg_it == load_store_reg->end()) {
    auto temp = code->cfg().allocate_temp();
    load_store_reg->emplace(l, temp);
    return gen_move(ig.get_node(l).type(), temp, l);
  } else {
    return gen_move(ig.get_node(l).type(), store_reg_it->second, l);
  }
}

// Store a LOAD instruction in block_load_info for each death in
// LiveOut(block) - LiveIn(succs) so that we can insert them later.
// Since there might exist situation of
//      B1: def s1
//        |
//       /|
//      / B2:def s2
//     |     last use s2
//     | /
//     B3: use s1
// Suppose s1 is to split around s2, s2 died on the edge of B2->B3
// Directly insert a load of s1 (suppose from s3) at the beginning of B3
// will result in the route of B1->B3 not knowing what s3 is (since store
// of s1 into s3 is inserted before def s2 which is in B2.) So what we will
// do is insert a block between B2 and B3:
//     B2->B4->B3
// where B4 does the loading of s1.
size_t split_for_block(const SplitPlan& split_plan,
                       const SplitCosts& split_costs,
                       const LivenessDomain& live_out,
                       const LivenessFixpointIterator& fixpoint_iter,
                       const Graph& ig,
                       cfg::Block* block,
                       std::unordered_map<vreg_t, vreg_t>* load_store_reg,
                       IRCode* code,
                       BlockLoadInfo* block_load_info) {
  auto& cfg = code->cfg();
  size_t split_move = 0;
  for (auto& succ : block->succs()) {
    LivenessDomain live_in = fixpoint_iter.get_live_in_vars_at(succ->target());
    for (auto reg : live_out.elements()) {
      if (live_in.contains(reg)) {
        continue;
      }
      auto split_it = split_plan.split_around.find(reg);
      if (split_it == split_plan.split_around.end()) {
        continue;
      }
      // For each live range l split around reg.
      for (auto l : split_it->second) {
        if (!live_in.contains(l)) {
          continue;
        }
        IRInstruction* mov = gen_load_for_split(ig, l, load_store_reg, code);
        // Storing the mov needed to be inserted between blocks and
        // insert them together later.
        // If all of blocks's preds have reg died on the edge
        // pred->s, then no need to insert a block for every edge,
        // Inserting loads at the beginning of blocks will be fine and
        // won't cause problems.
        bool can_insert_directly =
            split_costs.death_at_other(reg).at(succ->target()) ==
            succ->target()->preds().size();
        if ((succ->type() == cfg::EDGE_GOTO ||
             succ->type() == cfg::EDGE_BRANCH) &&
            can_insert_directly) {
          // Use other_loaded_regs to make sure we don't load a register
          // several times in the same place.
          auto other_loaded_it =
              block_load_info->other_loaded_regs.find(succ->target());
          if (other_loaded_it == block_load_info->other_loaded_regs.end() ||
              other_loaded_it->second.find(l) ==
                  other_loaded_it->second.end()) {
            // Get first opcode instruction and insert move
            // before it.
            auto succ_block = succ->target();
            auto pos_it = succ_block->get_first_insn();
            cfg.insert_before(succ_block->to_cfg_instruction_iterator(pos_it),
                              mov);
            block_load_info->other_loaded_regs[succ->target()].emplace(l);
            ++split_move;
          }
          continue;
        }

        auto block_edge =
            std::pair<cfg::Block*, cfg::Block*>(block, succ->target());
        auto lastmei = block->rbegin();
        // Because in find_split we limited the try-catch edge to only deal
        // with catch block where reg died on all the exception edge toward it.
        // So even if there is a EDGE_GOTO we don't need to worry about should
        // we insert a block to load reg or not.
        if (succ->type() == cfg::EDGE_THROW) {
          // Try Catch blocks.
          // Use try_loaded_regs to make sure we don't load a register several
          // times in the same place.
          auto try_loaded_it =
              block_load_info->try_loaded_regs.find(succ->target());
          if (try_loaded_it == block_load_info->try_loaded_regs.end() ||
              try_loaded_it->second.find(l) == try_loaded_it->second.end()) {
            block_load_info->mode_and_insn[block_edge].add_insn_mode(mov,
                                                                     TRYCATCH);
            block_load_info->try_loaded_regs[succ->target()].emplace(l);
          }
        } else {
          always_assert(succ->type() == cfg::EDGE_GOTO ||
                        succ->type() == cfg::EDGE_BRANCH);
          block_load_info->mode_and_insn[block_edge].add_insn_mode(mov, BRANCH);
        }
      }
    }
  }
  return split_move;
}

// For each define of a reg,
// insert a store for all live range l split around reg
// before define of reg.
size_t split_for_define(const SplitPlan& split_plan,
                        const Graph& ig,
                        const IRInstruction* insn,
                        const LivenessDomain& live_out,
                        IRCode* code,
                        std::unordered_map<vreg_t, vreg_t>* load_store_reg,
                        cfg::InstructionIterator it) {
  auto& cfg = code->cfg();
  size_t split_move = 0;
  if (insn->has_dest()) {
    auto dest = insn->dest();
    // Avoid case like:
    //  Def s0
    //  Add s0 s0 s1
    // and store instruction were inserted twice
    bool dest_not_src = true;
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      if (dest == insn->src(i)) {
        dest_not_src = false;
      }
    }
    auto split_it = split_plan.split_around.find(dest);
    if (split_it != split_plan.split_around.end() && dest_not_src) {
      if (opcode::is_a_move_result(insn->opcode())) {
        // Move-result must follow instruction that write
        // result register, so insert before invoke-xxx or
        // filled-new-array instead.
        it = cfg.primary_instruction_of_move_result(it);
        always_assert(!it.is_end());
      }
      for (auto l : split_it->second) {
        if (!live_out.contains(l)) {
          continue;
        }
        IRInstruction* mov = gen_store_for_split(ig, l, load_store_reg, code);
        cfg.insert_before(it, mov);
        ++split_move;
      }
    }
  }
  return split_move;
}

// For each death of a reg,
// insert a load for all live range l split around reg
// after death of reg.
size_t split_for_last_use(const SplitPlan& split_plan,
                          const Graph& ig,
                          const IRInstruction* insn,
                          const LivenessDomain& live_out,
                          cfg::Block* block,
                          IRCode* code,
                          std::unordered_map<vreg_t, vreg_t>* load_store_reg,
                          IRList::reverse_iterator& it,
                          BlockLoadInfo* block_load_info) {
  auto& cfg = code->cfg();
  size_t split_move = 0;
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    auto src = insn->src(i);
    if (live_out.contains(src)) {
      continue;
    }
    auto split_it = split_plan.split_around.find(src);
    if (split_it != split_plan.split_around.end()) {
      for (auto l : split_it->second) {
        if (!live_out.contains(l)) {
          continue;
        }
        // There is a situation:
        //   B10: ...
        //        if_xx v0 ->b12
        //   B11: ...
        //   B12: ...
        // where v0 is the value to be split around, and this is its last
        // use, which may fall into this category, then the code would do
        // insert loads after if_xx, but after cfg was rebuilt, the loads
        // will go into B11 but not both B11 and B12, so if this occurred,
        // we would treat it like the case of
        // live_out(block) - live_in(succ_block).
        if (opcode::is_branch(insn->opcode()) && it == block->rbegin()) {
          for (auto& succ : block->succs()) {
            IRInstruction* mov =
                gen_load_for_split(ig, l, load_store_reg, code);
            auto block_edge =
                std::pair<cfg::Block*, cfg::Block*>(block, succ->target());
            if (succ->type() == cfg::EDGE_BRANCH ||
                succ->type() == cfg::EDGE_GOTO) {
              // Branches or GOTO, need to change target.
              block_load_info->mode_and_insn[block_edge].add_insn_mode(mov,
                                                                       BRANCH);
            }
          }
          continue;
        }

        IRInstruction* mov = gen_load_for_split(ig, l, load_store_reg, code);
        if (opcode::writes_result_register(insn->opcode()) &&
            it.base()->type == MFLOW_OPCODE &&
            opcode::is_a_move_result(it.base()->insn->opcode())) {
          // Move-result must follow instruction that write
          // result register, so insert after move-result instead.
          cfg.insert_after(block->to_cfg_instruction_iterator(it.base()), mov);
        } else {
          cfg.insert_after(block->to_cfg_instruction_iterator(--(it.base())),
                           mov);
          ++it;
        }
        ++split_move;
      }
    }
  }
  return split_move;
}

// Insert the insn stored in block_load_info by:
//    1. Inserting a new block with the insn between two blocks.
// or 2. Just inserting at beginning of a block.
size_t insert_insn_between_blocks(const BlockLoadInfo& block_load_info,
                                  IRCode* code) {
  auto& cfg = code->cfg();
  size_t split_move = 0;
  for (auto& pair : block_load_info.mode_and_insn) {
    auto block = pair.first.first;
    auto s = pair.first.second; // second block
    BlockMode block_mode = pair.second.block_mode;
    if (block_mode == TRYCATCH) {
      // Two blocks are connected by TRYCATCH edge.
      // Since we made sure in find_split that for each split the catch block
      // would understand the load instruction from every TRYCATCH edge to it,
      // So just iterate to the first opcode instruction in catch block and
      // insert move before it.
      auto pos_it = s->get_first_insn();
      auto cfg_pos_it = s->to_cfg_instruction_iterator(pos_it);
      // move-exception should be the first instruction in exception handler.
      if (!cfg_pos_it.is_end_in_block() &&
          cfg_pos_it->insn->opcode() == OPCODE_MOVE_EXCEPTION) {
        cfg_pos_it.move_next_in_block();
      }

      for (auto insn : pair.second.block_insns) {
        cfg.insert_before(cfg_pos_it, insn);
        ++split_move;
      }
    } else if (block_mode == BRANCH) {
      // Two blocks are connected by BRANCH/GOTO edge, so we need to insert
      // another block and redirect edges. suppose previously it was B1->B2
      // (there could be more than one edges). after split, for each edge
      // between B1 and B2, it should become
      //    B1 -> B3 -> B2

      // Create a new block containing all the load instructions.
      cfg::Block* new_block = cfg.create_block();
      for (auto insn : pair.second.block_insns) {
        new_block->push_back(insn);
        ++split_move;
      }
      // Insert 'new_block' between 'block' and 's'.
      cfg.insert_block(block, s, new_block);
    }
  }
  return split_move;
}

// Live range splitting, Theory from
// K. Cooper & L. Simpson. Live Range Splitting in a Graph Coloring
// Register Allocator.
size_t split(const LivenessFixpointIterator& fixpoint_iter,
             const SplitPlan& split_plan,
             const SplitCosts& split_costs,
             const Graph& ig,
             IRCode* code) {
  // Keep track of which reg is stored or loaded to which temp
  // so that we can get the right reg loaded or stored.
  std::unordered_map<vreg_t, vreg_t> load_store_reg;
  BlockLoadInfo block_load_info;
  size_t split_move = 0;
  auto& cfg = code->cfg();

  for (cfg::Block* block : cfg.blocks()) {
    LivenessDomain live_out = fixpoint_iter.get_live_out_vars_at(block);
    // Split for death of reg on edge from block to its succs blocks.
    split_move += split_for_block(split_plan,
                                  split_costs,
                                  live_out,
                                  fixpoint_iter,
                                  ig,
                                  block,
                                  &load_store_reg,
                                  code,
                                  &block_load_info);
    // For each instruction in block in reverse order
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type != MFLOW_OPCODE) {
        continue;
      }
      // Split for define and last use of reg.
      auto insn = it->insn;
      auto cfg_it = block->to_cfg_instruction_iterator(--(it.base()));
      split_move += split_for_define(split_plan, ig, insn, live_out, code,
                                     &load_store_reg, cfg_it);

      split_move += split_for_last_use(split_plan,
                                       ig,
                                       insn,
                                       live_out,
                                       block,
                                       code,
                                       &load_store_reg,
                                       it,
                                       &block_load_info);
      // Update live_out.
      fixpoint_iter.analyze_instruction(it->insn, &live_out);
    }
  }

  // Insert new blocks or instructions for live range dead on edge.
  split_move += insert_insn_between_blocks(block_load_info, code);
  return split_move;
}

} // namespace regalloc
