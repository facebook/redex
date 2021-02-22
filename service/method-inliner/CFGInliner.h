/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"

#include <boost/optional.hpp>

namespace cfg {

class CFGInlinerPlugin;
class CFGInliner {
 public:
  /*
   * Copy callee's blocks into caller: uses default plugin, and insertion.
   * Expects callsite to be a method call from caller.
   * Registers starting with next_caller_reg must be available.
   */
  static void inline_cfg(ControlFlowGraph* caller,
                         const cfg::InstructionIterator& callsite,
                         const ControlFlowGraph& callee,
                         size_t next_caller_reg);

  /*
   * Copy callee's blocks into caller:
   * Uses provided plugin to update caller and/or copy of callee
   */
  static void inline_cfg(ControlFlowGraph* caller,
                         const cfg::InstructionIterator& inline_site,
                         const ControlFlowGraph& callee,

                         size_t next_caller_reg,
                         CFGInlinerPlugin& plugin);

 private:
  /*
   * Prepares the CFG for inlining by removing a subset of MFLOW_DEBUG
   * instructions that would make no sense to be duplicated.
   */
  static void cleanup_callee_debug(ControlFlowGraph* cfg);

  /*
   * A "ghost block" may be added for CFGs that have multiple exit blocks and
   * EDGE_GHOST edges are added from all exit points to this one (empty) ghost
   * block. This block gets in the way while inlining. Remove if it's there.
   */
  static void remove_ghost_exit_block(ControlFlowGraph* cfg);

  /*
   * If `it` isn't already, make it the last instruction of its block
   */
  static std::pair<Block*, Block*> maybe_split_block(
      ControlFlowGraph* caller, const InstructionIterator& it);

  /*
   * If `it` isn't first, make it the first instruction of its block
   */
  static std::pair<Block*, Block*> maybe_split_block_before(
      ControlFlowGraph* caller, const InstructionIterator& it);

  /*
   * Change the register numbers to not overlap with caller.
   */
  static void remap_registers(ControlFlowGraph* callee, reg_t next_caller_reg);

  /*
   * Move ownership of blocks and edges from callee to caller
   */
  static void steal_contents(ControlFlowGraph* caller,
                             Block* callsite,
                             ControlFlowGraph* callee);

  /*
   * If `insert_after`, add edges from callsite to the entry point and back from
   * the exit points to to the block after the callsite. Otherwise add edges
   * into callsite to the entry point and from the exit points to the block
   * after.
   */
  static void connect_cfgs(bool inline_after,
                           ControlFlowGraph* cfg,
                           Block* callsite,
                           const std::vector<Block*>& callee_blocks,
                           Block* callee_entry,
                           const std::vector<Block*>& callee_exits,
                           Block* callsite_split);

  /*
   * Convert load-params to moves, from a set of sources.
   */
  static void move_arg_regs(ControlFlowGraph* callee,
                            const std::vector<reg_t>& srcs);

  /*
   * Convert returns to moves.
   */
  static void move_return_reg(ControlFlowGraph* callee,
                              const boost::optional<reg_t>& ret_reg);

  /*
   * Callees that were not in a try region when their CFGs were created, need to
   * have some blocks split because the callsite is in a try region. We do this
   * because we need to add edges from the throwing opcodes to the catch handler
   * of the caller's try region.
   *
   * Assumption: callsite is in a try region
   */
  static void split_on_callee_throws(ControlFlowGraph* callee);

  /*
   * Add a throw edge from each may_throw to each catch that is thrown to from
   * the callsite
   *   * If there are already throw edges in callee, add this edge to the end
   *     of the list
   *
   * Assumption: caller_catches is sorted by catch index
   */
  static void add_callee_throws_to_caller(
      ControlFlowGraph* cfg,
      const std::vector<Block*>& callee_blocks,
      const std::vector<Edge*>& caller_catches);

  /*
   * Set the parent pointers of the positions in `callee` to `callsite_dbg_pos`
   */
  static void set_dbg_pos_parents(ControlFlowGraph* callee,
                                  DexPosition* callsite_dbg_pos);

  /*
   * Return the equivalent move opcode for the given return opcode
   */
  static IROpcode return_to_move(IROpcode op);
};

/*
 * A base plugin to extend the capabilities of the CFG Inliner
 * An extension of CFGInlinerPlugin can modify either the caller
 * or a copy of the callee before and after the registers are
 * remapped, can provide register sources for the callee parameters,
 * and control whether the callee is inlined before or after the
 * provided instruction iterator, and whether instructions are removed
 * from the caller.
 */
class CFGInlinerPlugin {
 public:
  virtual ~CFGInlinerPlugin() = default;
  // Will be called before any of caller or callee's registers have changed
  // Override this method to modify either after the copy is made and before
  // any registers are adjusted.
  //
  // Returns `true` if registers have been added.
  virtual bool update_before_reg_remap(ControlFlowGraph*, ControlFlowGraph*) {
    return false;
  }
  // Will be called after both register remap and load parameter to move have
  // changed callee, but before callee's blocks are merged into caller.
  // Override to modify either before the merge occurs.
  //
  // Returns `true` if registers have been added and a full recompute
  // of registers is needed. Recomputing registers requires a full traversal of
  // the CFG, which is expensive. Avoid if possible.
  virtual bool update_after_reg_remap(ControlFlowGraph*, ControlFlowGraph*) {
    return false;
  }

  // Optionally provide a set of registers for the sources of callee's
  // parameters If none is returned, inliner extracts registers from the sources
  // of the instruction within the instruction iterator
  virtual boost::optional<const std::vector<reg_t>&> inline_srcs() {
    return boost::none;
  }

  // Optionally provide a register from caller to move a returned value from
  // callee into when combining blocks. Leaving this as none, if the
  // instruction iterator's instruction has a move result, this register
  // will be used instead. If it does not have a move result, the value will
  // be discarded on 'return'.
  virtual boost::optional<reg_t> reg_for_return() { return boost::none; }

  // Overriding this to return false will cause callee's blocks to be inserted
  // before the instruction of the instruction iterator, instead of after
  virtual bool inline_after() { return true; }

  // Overriding this to return false will retain the instruction of the
  // instruction iterator, whereas by default the instruction and any associated
  // move result will be deleted.
  virtual bool remove_inline_site() { return true; }
};

} // namespace cfg
