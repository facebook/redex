/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CFGInliner.h"

#include <memory>

#include "IRList.h"
#include "IROpcode.h"
#include "DexPosition.h"

namespace cfg {

// TODO:
//  * should this really be a friend class to ControlFlowGraph, Block, and Edge?

/*
 * Copy callee's blocks into caller
 */
void CFGInliner::inline_cfg(ControlFlowGraph* caller,
                            const InstructionIterator& callsite,
                            const ControlFlowGraph& callee_orig) {
  always_assert(&callsite.cfg() == caller);

  // copy the callee because we're going to move its contents into the caller
  ControlFlowGraph callee;
  callee_orig.deep_copy(&callee);

  TRACE(CFG, 3, "caller %s\n", SHOW(*caller));
  TRACE(CFG, 3, "callee %s\n", SHOW(callee));

  if (caller->get_succ_edge_of_type(callsite.block(), EDGE_THROW) != nullptr) {
    split_on_callee_throws(&callee);
  }

  // we save these blocks here because we're going to empty out the callee CFG
  const auto& callee_entry_block = callee.entry_block();
  const auto& callee_return_blocks = callee.return_blocks();

  // make the invoke last of its block
  Block* after_callee = maybe_split_block(caller, callsite);
  TRACE(CFG, 3, "caller after split %s\n", SHOW(*caller));

  DexPosition* callsite_dbg_pos = get_dbg_pos(callsite);
  if (callsite_dbg_pos) {
    set_dbg_pos_parents(&callee, callsite_dbg_pos);
    // ensure that the caller's code after the inlined method retain their
    // original position
    const auto& first = after_callee->begin();
    if (first == after_callee->end() || first->type != MFLOW_POSITION) {
      // but don't add if there's already a position at the front of this
      // block
      after_callee->m_entries.push_front(
          *(new MethodItemEntry(std::make_unique<DexPosition>(*callsite_dbg_pos))));
    }
  }

  // make sure the callee's registers don't overlap with the caller's
  auto callee_regs_size = callee.get_registers_size();
  auto caller_regs_size = caller->get_registers_size();
  remap_registers(&callee, caller_regs_size);

  move_arg_regs(&callee, callsite->insn);
  const cfg::InstructionIterator& move_res = caller->move_result_of(callsite);
  move_return_reg(&callee,
                  move_res.is_end()
                      ? boost::none
                      : boost::optional<uint16_t>{move_res->insn->dest()});

  TRACE(CFG, 3, "callee after remap %s\n", SHOW(callee));

  // delete the move-result before connecting the cfgs because it's in a block
  // that may be merged into another
  if (!move_res.is_end()) {
    caller->remove_insn(move_res);
  }

  // redirect to callee
  const std::vector<Block*> callee_blocks = callee.blocks();
  steal_contents(caller, callsite.block(), &callee);
  connect_cfgs(caller, callsite.block(), callee_blocks, callee_entry_block,
               callee_return_blocks, after_callee);
  caller->set_registers_size(callee_regs_size + caller_regs_size);

  TRACE(CFG, 3, "caller after connect %s\n", SHOW(*caller));

  // delete the invoke after connecting the CFGs because remove_insn will
  // remove the outgoing throw if we remove the callsite
  caller->remove_insn(callsite);

  if (ControlFlowGraph::DEBUG) {
    caller->sanity_check();
  }
  TRACE(CFG, 3, "final %s\n", SHOW(*caller));
}

/*
 * If it isn't already, make `it` the last instruction of its block
 * return the block that should be run after the callee
 */
Block* CFGInliner::maybe_split_block(ControlFlowGraph* caller,
                                     const InstructionIterator& it) {
  always_assert(caller->editable());
  always_assert(!it.block()->empty());

  const IRList::iterator& raw_it = it.unwrap();
  Block* old_block = it.block();
  if (raw_it != old_block->get_last_insn()) {
    caller->split_block(it);
  }

  // The call is already the last instruction of the block.
  // No need to change the code, just return the next block
  Edge* goto_edge = caller->get_succ_edge_of_type(old_block, EDGE_GOTO);
  always_assert(goto_edge != nullptr);
  return goto_edge->target();
}

/*
 * Change the register numbers to not overlap with caller.
 */
void CFGInliner::remap_registers(cfg::ControlFlowGraph* callee,
                                 uint16_t caller_regs_size) {
  for (auto& mie : cfg::InstructionIterable(*callee)) {
    auto insn = mie.insn;
    for (uint16_t i = 0; i < insn->srcs_size(); ++i) {
      insn->set_src(i, insn->src(i) + caller_regs_size);
    }
    if (insn->dests_size()) {
      insn->set_dest(insn->dest() + caller_regs_size);
    }
  }
}

/*
 * Move ownership of blocks and edges from callee to caller
 */
void CFGInliner::steal_contents(ControlFlowGraph* caller,
                                Block* callsite,
                                ControlFlowGraph* callee) {
  always_assert(!caller->m_blocks.empty());
  for (auto& entry : callee->m_blocks) {
    Block* b = entry.second;
    b->m_parent = caller;
    size_t id = caller->m_blocks.rbegin()->first + 1;
    b->m_id = id;
    caller->m_blocks.emplace(id, b);
  }
  callee->m_blocks.clear();

  // transfer ownership of the edges
  caller->m_edges.reserve(caller->m_edges.size() + callee->m_edges.size());
  caller->m_edges.insert(callee->m_edges.begin(), callee->m_edges.end());
  callee->m_edges.clear();
}

/*
 * Add edges from callsite to the entry point and back from the exit points to
 * to the block after the callsite
 */
void CFGInliner::connect_cfgs(ControlFlowGraph* cfg,
                              Block* callsite,
                              const std::vector<Block*>& callee_blocks,
                              Block* callee_entry,
                              const std::vector<Block*>& callee_exits,
                              Block* after_callsite) {

  // Add edges from callee throw sites to caller catch sites
  const auto& caller_throws = callsite->get_outgoing_throws_in_order();

  if (!caller_throws.empty()) {
    add_callee_throws_to_caller(cfg, callee_blocks, caller_throws);
  }

  // Remove the goto between the callsite and its successor
  cfg->delete_succ_edge_if(
      callsite, [](const Edge* e) { return e->type() == EDGE_GOTO; });

  auto connect = [&cfg](std::vector<Block*> preds, Block* succ) {
    for (Block* pred : preds) {
      TRACE(CFG, 4, "connecting %d, %d in %s\n", pred->id(), succ->id(), SHOW(*cfg));
      cfg->add_edge(pred, succ, EDGE_GOTO);
      // If this is the only connecting edge, we can merge these blocks into one
      if (preds.size() == 1 && succ->preds().size() == 1 &&
          cfg->blocks_are_in_same_try(pred, succ)) {
        // FIXME: this is annoying because it destroys the succ block
        // (invalidating any iterators into it). Maybe it would be better to do
        // this during cfg.simplify at the very end?
        cfg->merge_blocks(pred, succ);
      }
    }
  };

  // We must connect the return first because merge_blocks may delete the
  // successor
  // TODO: tail call optimization (if after_callsite is just a return)
  connect(callee_exits, after_callsite);
  connect({callsite}, callee_entry);
}

/*
 * Convert load-params to moves.
 */
void CFGInliner::move_arg_regs(cfg::ControlFlowGraph* callee,
                               const IRInstruction* invoke) {
  auto param_insns = callee->get_param_instructions();

  uint16_t i = 0;
  for (auto& mie : ir_list::InstructionIterable(param_insns)) {
    IRInstruction* load = mie.insn;
    IRInstruction* move =
        new IRInstruction(opcode::load_param_to_move(mie.insn->opcode()));
    always_assert(i < invoke->srcs_size());
    move->set_src(0, invoke->src(i));
    move->set_dest(load->dest());
    ++i;

    // replace the load instruction with the new move instruction
    mie.insn = move;
    delete load;
  }
}

/*
 * Convert returns to moves.
 */
void CFGInliner::move_return_reg(cfg::ControlFlowGraph* callee,
                                 const boost::optional<uint16_t>& ret_reg) {
  std::vector<cfg::InstructionIterator> to_delete;
  auto iterable = cfg::InstructionIterable(*callee);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (is_return(it->insn->opcode())) {
      IRInstruction* ret = it->insn;
      auto op = return_to_move(ret->opcode());

      if (op != OPCODE_NOP && ret_reg) {
        IRInstruction* move = new IRInstruction(op);
        move->set_src(0, ret->src(0));
        move->set_dest(*ret_reg);
        it->insn = move;
        delete ret;
      } else {
        // return-void is equivalent to nop
        // or the return register isn't used in the caller
        to_delete.push_back(it);
      }
    }
  }

  for (auto& it : to_delete) {
    callee->remove_insn(it);
  }
}

/*
 * Callees that were not in a try region when their CFGs were created, need to
 * have some blocks split because the callsite is in a try region. We do this
 * because we need to add edges from the throwing opcodes to the catch handler
 * of the caller's try region.
 *
 * Assumption: callsite is in a try region
 */
void CFGInliner::split_on_callee_throws(ControlFlowGraph* callee) {
  std::vector<Block*> work_list = callee->blocks();
  // iterate with an index instead of an iterator because we're adding to the
  // end while we iterate
  for (uint32_t i = 0; i < work_list.size(); ++i) {
    Block* b = work_list[i];
    // look for blocks we need to split
    IRList::iterator last = b->get_last_insn();
    const auto& iterable = ir_list::InstructionIterable(*b);
    for (auto it = iterable.begin(); it != iterable.end(); ++it) {
      const auto& mie = *it;
      const auto insn = mie.insn;
      const auto op = insn->opcode();
      if (opcode::can_throw(op) && it.unwrap() != last) {
        const auto& cfg_it = b->to_cfg_instruction_iterator(it);
        Block* new_block = callee->split_block(cfg_it);
        work_list.push_back(new_block);
      }
    }
  }
}

/*
 * Add a throw edge from each may_throw to each catch that is thrown to from the
 * callsite
 *   * If there are already throw edges in callee, add this edge to the end
 *     of the list
 *
 * Assumption: caller_catches is sorted by catch index
 */
void CFGInliner::add_callee_throws_to_caller(
    ControlFlowGraph* cfg,
    const std::vector<Block*>& callee_blocks,
    const std::vector<Edge*>& caller_catches) {

  // There are two requirements about the catch indices here:
  //   1) New throw edges must be added to the end of a callee's existing throw
  //   chain. This is ensured by using the max index of the already existing
  //   throws
  //   2) New throw edges must go to the callsite's catch blocks in the same
  //   order that the existing catch chain does. This is ensured by sorting
  //   caller_catches by their throw indices.

  // Add throw edges from callee_block to all the caller catches
  const auto& add_throw_edges =
      [&cfg, &caller_catches](Block* callee_block, uint32_t starting_index) {
        auto index = starting_index;
        for (Edge* caller_catch : caller_catches) {
          cfg->add_edge(callee_block, caller_catch->target(),
                        caller_catch->m_throw_info->catch_type, index);
          ++index;
        }
      };

  for (Block* callee_block : callee_blocks) {
    const auto& existing_throws = callee_block->get_outgoing_throws_in_order();
    if (existing_throws.empty()) {
      // Blocks that end in a throwing instruction but don't have outgoing throw
      // instructions yet.
      //   * Instructions that can throw that were not in a try region before
      //   being inlined. These may have been created by split_on_callee_throws.
      //   * OPCODE_THROW instructions without any catch blocks before being
      //   inlined.
      IRList::iterator last = callee_block->get_last_insn();
      if (last != callee_block->end()) {
        const auto op = last->insn->opcode();
        if (opcode::can_throw(op)) {
          add_throw_edges(callee_block, /* starting_index */ 0);
        }
      }
    } else if (existing_throws.back()->m_throw_info->catch_type != nullptr) {
      // Blocks that throw already
      //   * Instructions that can throw that were already in a try region with
      //   catch blocks
      //   * But don't add to the end of a throw list if there's a catchall
      //   already
      add_throw_edges(callee_block,
                      existing_throws.back()->m_throw_info->index + 1);
    }
  }
}

void CFGInliner::set_dbg_pos_parents(ControlFlowGraph* callee,
                                     DexPosition* callsite_dbg_pos) {
  for (auto& entry : callee->m_blocks) {
    Block* b = entry.second;
    for (auto& mie : *b) {
      // Don't overwrite existing parent pointers because those are probably
      // methods that were inlined into callee before
      if (mie.type == MFLOW_POSITION && mie.pos->parent == nullptr) {
        mie.pos->parent = callsite_dbg_pos;
      }
    }
  }
}

/*
 * Return the equivalent move opcode for the given return opcode
 */
IROpcode CFGInliner::return_to_move(IROpcode op) {
  switch (op) {
  case OPCODE_RETURN_VOID:
    return OPCODE_NOP;
  case OPCODE_RETURN:
    return OPCODE_MOVE;
  case OPCODE_RETURN_WIDE:
    return OPCODE_MOVE_WIDE;
  case OPCODE_RETURN_OBJECT:
    return OPCODE_MOVE_OBJECT;
  default:
    always_assert_log(false, "Expected return op, got %s", SHOW(op));
    not_reached();
  }
}

DexPosition* CFGInliner::get_dbg_pos(const cfg::InstructionIterator& callsite) {
  auto search_block = [](Block* b,
                         IRList::iterator in_block_it) -> DexPosition* {
    // Search for an MFLOW_POSITION preceding this instruction within the
    // same block
    while (in_block_it->type != MFLOW_POSITION && in_block_it != b->begin()) {
      --in_block_it;
    }
    return in_block_it->type == MFLOW_POSITION ? in_block_it->pos.get()
                                               : nullptr;
  };
  auto result = search_block(callsite.block(), callsite.unwrap());
  if (result != nullptr) {
    return result;
  }

  // TODO: Positions should be connected to instructions rather than preceding
  // them in the flow of instructions. Having the positions depend on the order
  // of instructions is a very linear way to encode the information which isn't
  // very amenable to the editable CFG.

  // while there's a single predecessor, follow that edge
  const auto& cfg = callsite.cfg();
  std::function<DexPosition*(Block*)> check_prev_block;
  check_prev_block = [&check_prev_block, &search_block,
                      &cfg](Block* b) -> DexPosition* {
    const auto& reverse_gotos = cfg.get_pred_edges_of_type(b, EDGE_GOTO);
    if (b->preds().size() == 1 && !reverse_gotos.empty()) {
      Block* prev_block = reverse_gotos[0]->src();
      if (!prev_block->empty()) {
        auto result = search_block(prev_block, std::prev(prev_block->end()));
        if (result != nullptr) {
          return result;
        }
      }
      // Didn't find any MFLOW_POSITIONs in `prev_block`, keep going.
      return check_prev_block(prev_block);
    }
    // This block has no solo predecessors anymore. Nowhere left to search.
    return nullptr;
  };
  return check_prev_block(callsite.block());
}

} // namespace cfg
