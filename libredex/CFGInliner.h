/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"

#include <boost/optional.hpp>

namespace cfg {

class CFGInliner {
 public:
  /*
   * Copy callee's blocks into caller.
   */
  static void inline_cfg(ControlFlowGraph* caller,
                         const cfg::InstructionIterator& callsite,
                         const ControlFlowGraph& callee);

 private:
  /*
   * If it isn't already, make `it` the last instruction of its block
   */
  static Block* maybe_split_block(ControlFlowGraph* caller,
                                  const InstructionIterator& it);

  /*
   * Change the register numbers to not overlap with caller.
   */
  static void remap_registers(ControlFlowGraph* callee,
                              uint16_t caller_regs_size);

  /*
   * Move ownership of blocks and edges from callee to caller
   */
  static void steal_contents(ControlFlowGraph* caller,
                             Block* callsite,
                             ControlFlowGraph* callee);

  /*
   * Add edges from callsite to the entry point and back from the exit points to
   * to the block after the callsite
   */
  static void connect_cfgs(ControlFlowGraph* cfg,
                           Block* callsite,
                           const std::vector<Block*>& callee_blocks,
                           Block* callee_entry,
                           const std::vector<Block*>& callee_exits,
                           Block* after_callsite);

  /*
   * Convert load-params to moves.
   */
  static void move_arg_regs(ControlFlowGraph* callee,
                            const IRInstruction* invoke);

  /*
   * Convert returns to moves.
   */
  static void move_return_reg(ControlFlowGraph* callee,
                              const boost::optional<uint16_t>& ret_reg);

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

  /*
   * Find the first debug position preceding the callsite
   */
  static DexPosition* get_dbg_pos(const cfg::InstructionIterator& callsite);
};

} // namespace cfg
