/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CFGInliner.h"

#include <memory>

#include "DexPosition.h"
#include "IRList.h"
#include "IROpcode.h"
#include "Show.h"
#include "SourceBlocks.h"
#include "Trace.h"

namespace cfg {

// TODO:
//  * should this really be a friend class to ControlFlowGraph, Block, and Edge?

/*
 * Copy callee's blocks into caller
 */
void CFGInliner::inline_cfg(ControlFlowGraph* caller,
                            const InstructionIterator& callsite,
                            const ControlFlowGraph& callee_orig,
                            size_t next_caller_reg) {
  CFGInlinerPlugin base_plugin;
  inline_cfg(caller, callsite, callee_orig, next_caller_reg, base_plugin);
}

namespace {

boost::optional<float> get_source_blocks_factor(
    const InstructionIterator& inline_site,
    const ControlFlowGraph& callee_cfg) {
  auto caller_block = inline_site.block();
  float caller_val;
  {
    auto sb_vec = source_blocks::gather_source_blocks(caller_block);
    if (sb_vec.empty()) {
      return boost::none;
    }
    if (!sb_vec[0]->val) {
      return boost::none;
    }
    caller_val = *sb_vec[0]->val;
  }
  if (caller_val == 0) {
    return 0.0f;
  }

  // Assume that integrity is guaranteed, so that val at entry is
  // dominating all blocks.
  float callee_val;
  {
    auto sb_vec = source_blocks::gather_source_blocks(callee_cfg.entry_block());
    if (sb_vec.empty()) {
      return boost::none;
    }
    if (!sb_vec[0]->val) {
      return boost::none;
    }
    callee_val = *sb_vec[0]->val;
  }
  if (callee_val == 0) {
    return 0.0f;
  }

  // Expectation would be that callee_val >= caller_val. But tracking might
  // not be complete.

  // This will normalize to the value at the callsite.
  return caller_val / callee_val;
}

void normalize_source_blocks(ControlFlowGraph& cfg, float factor) {
  for (auto* b : cfg.blocks()) {
    auto sb_vec = source_blocks::gather_source_blocks(b);
    for (auto* sb : sb_vec) {
      if (sb->val) {
        *sb->val *= factor;
      }
    }
  }
}

} // namespace

void CFGInliner::inline_cfg(ControlFlowGraph* caller,
                            const InstructionIterator& inline_site,
                            const ControlFlowGraph& callee_orig,
                            size_t next_caller_reg,
                            CFGInlinerPlugin& plugin) {
  always_assert(&inline_site.cfg() == caller);

  // copy the callee because we're going to move its contents into the caller
  ControlFlowGraph callee;
  callee_orig.deep_copy(&callee);

  {
    auto sb_factor = get_source_blocks_factor(inline_site, callee_orig);
    if (sb_factor) {
      normalize_source_blocks(callee, *sb_factor);
    }
  }

  remove_ghost_exit_block(&callee);
  cleanup_callee_debug(&callee);

  TRACE(CFG, 3, "caller %s", SHOW(*caller));
  TRACE(CFG, 3, "callee %s", SHOW(callee));

  if (caller->get_succ_edge_of_type(inline_site.block(), EDGE_THROW) !=
      nullptr) {
    split_on_callee_throws(&callee);
  }

  // we save these blocks here because we're going to empty out the callee CFG
  const auto& callee_entry_block = callee.entry_block();
  const auto& callee_return_blocks = callee.return_blocks();

  bool inline_after = plugin.inline_after();

  // Find the closest dbg position for the inline site, if split before
  DexPosition* inline_site_dbg_pos =
      inline_after ? nullptr : inline_site.cfg().get_dbg_pos(inline_site);

  // make the invoke last of its block or first based on inline_after
  auto split = inline_after ? maybe_split_block(caller, inline_site)
                            : maybe_split_block_before(caller, inline_site);
  Block* split_on_inline = split.first;
  Block* callsite_blk = split.second;
  TRACE(CFG, 3, "split caller %s : %s", inline_after ? "after" : "before",
        SHOW(*caller));

  // Find the closest dbg position for the inline site, if split after
  inline_site_dbg_pos = inline_after
                            ? inline_site.cfg().get_dbg_pos(inline_site)
                            : inline_site_dbg_pos;

  if (inline_site_dbg_pos) {
    set_dbg_pos_parents(&callee, inline_site_dbg_pos);
    // ensure that the caller's code after the inlined method retain their
    // original position
    const auto& first = split_on_inline->begin();
    // inserting debug insn before param load insn does not work
    if (split_on_inline != inline_site.cfg().entry_block() &&
        (first == split_on_inline->end() || first->type != MFLOW_POSITION)) {
      // but don't add if there's already a position at the front of this
      // block
      split_on_inline->m_entries.push_front(*(new MethodItemEntry(
          std::make_unique<DexPosition>(*inline_site_dbg_pos))));
    }
  }

  if (plugin.update_before_reg_remap(caller, &callee)) {
    next_caller_reg = caller->get_registers_size();
  }

  // make sure the callee's registers don't overlap with the caller's
  auto callee_regs_size = callee.get_registers_size();
  auto old_caller_regs_size = caller->get_registers_size();
  always_assert(next_caller_reg <= old_caller_regs_size);
  remap_registers(&callee, next_caller_reg);

  auto alt_srcs = plugin.inline_srcs();
  move_arg_regs(&callee, alt_srcs ? *alt_srcs : inline_site->insn->srcs_vec());

  auto return_reg = plugin.reg_for_return();

  if (inline_site->insn->has_move_result_any()) {
    const cfg::InstructionIterator& move_res =
        caller->move_result_of(inline_site);
    return_reg = return_reg
                     ? return_reg
                     : (move_res.is_end()
                            ? boost::none
                            : boost::optional<reg_t>{move_res->insn->dest()});
    // delete the move-result if there is one to remove, before connecting the
    // cfgs because it's in a block that may be merged into another
    if (plugin.remove_inline_site() && !move_res.is_end()) {
      caller->remove_insn(move_res);
    }
  }
  move_return_reg(&callee, return_reg);
  TRACE(CFG, 3, "callee after remap %s", SHOW(callee));

  bool need_reg_size_recompute = plugin.update_after_reg_remap(caller, &callee);
  // redirect to callee
  const std::vector<Block*> callee_blocks = callee.blocks();
  steal_contents(caller, callsite_blk, &callee);
  connect_cfgs(inline_after, caller, callsite_blk, callee_blocks,
               callee_entry_block, callee_return_blocks, split_on_inline);
  if (need_reg_size_recompute) {
    caller->recompute_registers_size();
  } else {
    size_t needed_caller_regs_size = next_caller_reg + callee_regs_size;
    if (needed_caller_regs_size > old_caller_regs_size) {
      caller->set_registers_size(needed_caller_regs_size);
    }
  }

  TRACE(CFG, 3, "caller after connect %s", SHOW(*caller));

  if (plugin.remove_inline_site()) {
    // delete the invoke after connecting the CFGs because remove_insn will
    // remove the outgoing throw if we remove the callsite
    caller->remove_insn(inline_site);
  }

  if (ControlFlowGraph::DEBUG) {
    caller->sanity_check();
  }
  TRACE(CFG, 3, "final %s", SHOW(*caller));
}

void CFGInliner::cleanup_callee_debug(ControlFlowGraph* cfg) {
  std::unordered_set<reg_t> valid_regs;
  for (auto* block_it : cfg->order()) {
    block_it->cleanup_debug(valid_regs);
  }
}

void CFGInliner::remove_ghost_exit_block(ControlFlowGraph* cfg) {
  auto ext = cfg->exit_block();
  if (ext && cfg->get_pred_edge_of_type(ext, EDGE_GHOST)) {
    cfg->remove_block(ext);
    cfg->set_exit_block(nullptr);
  }
}

/*
 * If it isn't already, make `it` the last instruction of its block
 * return the block that should be run after the callee and blk containing
 * callsite as a pair.
 */
std::pair<Block*, Block*> CFGInliner::maybe_split_block(
    ControlFlowGraph* caller, const InstructionIterator& it) {
  always_assert(caller->editable());
  always_assert(!it.block()->empty());

  const IRList::iterator& raw_it = it.unwrap();
  Block* old_block = it.block();
  if (raw_it != old_block->get_last_insn()) {
    caller->split_block(it);
  }

  // The call is already the last instruction of the block.
  // No need to change the code, just return the next block
  Block* goto_block = old_block->goes_to();
  always_assert(goto_block != nullptr);
  return std::make_pair(goto_block, old_block);
}

// Insert a new block if needed to make `it` the first instruction of a block.
// return the block that should be run before the callee and the block that
// should contain the callsite as a pair.
std::pair<Block*, Block*> CFGInliner::maybe_split_block_before(
    ControlFlowGraph* caller, const InstructionIterator& it) {
  always_assert(caller->editable());
  always_assert(!it.block()->empty());

  const IRList::iterator& raw_it = it.unwrap();
  Block* old_block = it.block();
  auto preds = old_block->preds();
  if (raw_it == old_block->get_first_insn() && preds.size() == 1) {
    Block* single_pred_block = preds.at(0)->src();
    if (single_pred_block->succs().size() == 1) {
      // If we already have a block.
      return std::make_pair(single_pred_block, old_block);
    }
  }

  // Inject an instruction and then split so 'it' is first of block
  auto dummy_end_instruction = new IRInstruction(OPCODE_NOP);
  caller->insert_before(it, dummy_end_instruction);
  auto new_blk = caller->split_block(
      old_block, caller->find_insn(dummy_end_instruction).unwrap());
  return std::make_pair(old_block, new_blk);
}

/*
 * Change the register numbers to not overlap with caller.
 */
void CFGInliner::remap_registers(cfg::ControlFlowGraph* callee,
                                 reg_t next_caller_reg) {
  for (auto& mie : cfg::InstructionIterable(*callee)) {
    auto insn = mie.insn;
    for (reg_t i = 0; i < insn->srcs_size(); ++i) {
      insn->set_src(i, insn->src(i) + next_caller_reg);
    }
    if (insn->has_dest()) {
      insn->set_dest(insn->dest() + next_caller_reg);
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
 * If `insert_after`, add edges from callsite to the entry point and back from
 * the exit points to to the block after the callsite. Otherwise add edges
 * into callsite to the entry point and from the exit points to the block
 * after.
 */
void CFGInliner::connect_cfgs(bool inline_after,
                              ControlFlowGraph* cfg,
                              Block* callsite,
                              const std::vector<Block*>& callee_blocks,
                              Block* callee_entry,
                              const std::vector<Block*>& callee_exits,
                              Block* callsite_split) {

  // Add edges from callee throw sites to caller catch sites
  const auto& caller_throws = callsite->get_outgoing_throws_in_order();

  if (!caller_throws.empty()) {
    add_callee_throws_to_caller(cfg, callee_blocks, caller_throws);
  }

  auto connect = [&cfg](const std::vector<Block*>& preds, Block* succ) {
    for (Block* pred : preds) {
      TRACE(CFG, 4, "connecting %d, %d in %s", pred->id(), succ->id(),
            SHOW(*cfg));
      cfg->add_edge(pred, succ, EDGE_GOTO);
    }
  };

  if (inline_after) {
    // Remove the goto between the callsite and its successor
    cfg->delete_succ_edge_if(
        callsite, [](const Edge* e) { return e->type() == EDGE_GOTO; });
    connect({callsite}, callee_entry);
  } else {
    // Remove the preds into callsite, having moved them to entry
    cfg->delete_succ_edges(callsite_split);
    connect({callsite_split}, callee_entry);
  }
  // TODO: tail call optimization (if callsite_split is a return & inline_after)

  if (inline_after) {
    connect(callee_exits, callsite_split);
  } else {
    cfg->delete_pred_edges(callsite);
    connect(callee_exits, callsite);
  }
}

/*
 * Convert load-params to moves.
 */
void CFGInliner::move_arg_regs(cfg::ControlFlowGraph* callee,
                               const std::vector<reg_t>& srcs) {
  auto param_insns = callee->get_param_instructions();

  reg_t i = 0;
  for (auto& mie : ir_list::InstructionIterable(param_insns)) {
    IRInstruction* load = mie.insn;
    IRInstruction* move =
        new IRInstruction(opcode::load_param_to_move(mie.insn->opcode()));
    move->set_src(0, srcs.at(i));
    move->set_dest(load->dest());
    // replace the load instruction with the new move instruction
    mie.insn = move;
    i++;
    delete load;
  }
}

/*
 * Convert returns to moves.
 */
void CFGInliner::move_return_reg(cfg::ControlFlowGraph* callee,
                                 const boost::optional<reg_t>& ret_reg) {
  std::vector<cfg::InstructionIterator> to_delete;
  auto iterable = cfg::InstructionIterable(*callee);
  for (auto it = iterable.begin(); it != iterable.end(); ++it) {
    if (opcode::is_a_return(it->insn->opcode())) {
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
                        caller_catch->throw_info()->catch_type, index);
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
    } else if (existing_throws.back()->throw_info()->catch_type != nullptr) {
      // Blocks that throw already
      //   * Instructions that can throw that were already in a try region with
      //   catch blocks
      //   * But don't add to the end of a throw list if there's a catchall
      //   already
      add_throw_edges(callee_block,
                      existing_throws.back()->throw_info()->index + 1);
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
    not_reached_log("Expected return op, got %s", SHOW(op));
  }
}

} // namespace cfg
