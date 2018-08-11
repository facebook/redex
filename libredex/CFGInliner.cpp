/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CFGInliner.h"

#include "IROpcode.h"

namespace cfg {

// TODO:
//  * handle throws
//  * handle debug positions
//  * deep copy cfg
//  * should this really be a friend class to ControlFlowGraph, Block, and Edge?

/*
 * Move callee's blocks into caller. This method consumes callee cfg
 */
void CFGInliner::inline_cfg(ControlFlowGraph* caller,
                            const InstructionIterator& callsite,
                            ControlFlowGraph&& callee) {
  always_assert(&callsite.cfg() == caller);

  TRACE(CFG, 3, "caller %s\ncallee %s\n", SHOW(*caller), SHOW(callee));

  // we save these blocks here because we're going to empty out the callee CFG
  const auto& callee_entry_point = callee.entry_block();
  const auto& callee_exit_points = callee.real_exit_blocks();

  if (caller->get_succ_edge_of_type(callsite.block(), EDGE_THROW) != nullptr) {
    // TODO: need to re-split callee because it's all in a try region now (if it
    // wasn't already)
    // How?
    //  * Destroy, insert try at beginning and end, rebuild?
    //  * scan for cases and split blocks, adding throw edges
    // TODO:
    //  * add throw edge from each may_throw to each catch in caller (at invoke)
    //    * If there are already throw edges in callee, add this edge to the end
    //      of the list
  }

  // make the invoke last of its block
  Block* after_callee = maybe_split_block(caller, callsite);
  TRACE(CFG, 3, "caller after split %s\n", SHOW(*caller));

  // make sure the callee's registers don't overlap with the caller's
  caller->recompute_registers_size();
  remap_registers(&callee, caller->get_registers_size());

  move_arg_regs(&callee, callsite->insn);
  const cfg::InstructionIterator& move_res = caller->move_result_of(callsite);
  move_return_reg(&callee,
                  move_res.is_end()
                      ? boost::none
                      : boost::optional<uint16_t>{move_res->insn->dest()});

  auto callee_regs_size = callee.get_registers_size();
  TRACE(CFG, 3, "callee after remap %s\n", SHOW(callee));

  // delete the invoke and move-result
  caller->remove_opcode(callsite);
  if (!move_res.is_end()) {
    caller->remove_opcode(move_res);
  }

  // redirect to callee
  steal_contents(caller, callsite.block(), &callee);
  connect_cfgs(caller, callsite.block(), callee_entry_point, callee_exit_points,
               after_callee);
  caller->set_registers_size(callee_regs_size);

  // TODO: destruct callee. Is this already happenning at end of function?

  TRACE(CFG, 3, "caller before simplify %s\n", SHOW(*caller));
  caller->simplify();
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
  if (raw_it != it.block()->get_last_insn()) {
    Block* new_block = caller->create_block();
    // move the rest of the instructions after the callsite into the new block
    new_block->m_entries.splice_selection(new_block->begin(),
                                          old_block->m_entries,
                                          std::next(raw_it), old_block->end());
    // make the outgoing edges come from the new block
    std::vector<Edge*> to_move(old_block->succs().begin(),
                               old_block->succs().end());
    for (auto e : to_move) {
      caller->set_edge_source(e, new_block);
    }
    // connect the halves of the block we just split up. We do this to maintain
    // a well-formed CFG for now even though we'll remove this edge later (when
    // we inline the callee)
    caller->add_edge(old_block, new_block, EDGE_GOTO);
    return new_block;
  }

  // the call is already the last instruction of the block and there aren't any
  // outgoing throws. No need to change the code, just return the next block
  always_assert(old_block->succs().size() == 1);
  Edge* goto_edge = caller->get_succ_edge_of_type(old_block, EDGE_GOTO);
  always_assert(goto_edge != nullptr);
  return goto_edge->target();
}

/*
 * Change the register numbers to not overlap with caller.
 * Convert load param and return instructions to move instructions.
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
  // Make space in the caller's list of blocks because the cfg linearizes in ID
  // order.
  //
  // This isn't required for correctness, but rather I expect this to
  // perform better. In the future, the CFG will choose a smart order when
  // linearizing, but right now it is lazy and just uses ID order.
  // TODO: When ControlFlowGraph::order() is smart, this can be simplified.
  std::vector<Block*> add_back_to_caller;
  for (auto it = caller->m_blocks.begin(); it != caller->m_blocks.end();) {
    Block* b = it->second;
    if (b->id() > callsite->id()) {
      b->m_id = b->m_id + callee->num_blocks();
      add_back_to_caller.push_back(b);
      it = caller->m_blocks.erase(it);
    } else {
      ++it;
    }
  }

  // Transfer ownership of the blocks and renumber their IDs.
  // The id's are chosen to be immediately after the callsite.
  uint32_t id = callsite->id() + 1;
  for (Block* b : callee->blocks()) {
    b->m_parent = caller;

    b->m_id = id;
    caller->m_blocks.emplace(id, b);
    ++id;
  }
  callee->m_blocks.clear();

  for (Block* b : add_back_to_caller) {
    caller->m_blocks.emplace(b->id(), b);
  }

  // transfer ownership of the edges
  for (Edge* e : callee->m_edges) {
    caller->m_edges.insert(e);
  }
  callee->m_edges.clear();
}

/*
 * Add edges from callsite to the entry point and back from the exit points to
 * to the block after the callsite
 */
void CFGInliner::connect_cfgs(ControlFlowGraph* cfg,
                              Block* callsite,
                              Block* callee_entry,
                              std::vector<Block*> callee_exits,
                              Block* after_callsite) {
  // First remove the goto between the callsite and its successor
  cfg->delete_succ_edge_if(
      callsite, [](const Edge* e) { return e->type() == EDGE_GOTO; });

  auto connect = [&cfg](std::vector<Block*> preds, Block* succ) {
    for (Block* pred : preds) {
      cfg->add_edge(pred, succ, EDGE_GOTO);
      // If this is the only connecting edge, we can merge these blocks into one
      if (preds.size() == 1 && cfg->blocks_are_in_same_try(pred, succ)) {
        cfg->merge_blocks(pred, succ);
      }
    }
  };

  // We must connect the return first because merge blocks may delete the
  // successor
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
    callee->remove_opcode(it);
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


} // namespace cfg
