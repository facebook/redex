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
   * Convert load param and return instructions to move instructions.
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
                           Block* callee_entry,
                           std::vector<Block*> callee_exits,
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
   * Return the equivalent move opcode for the given return opcode
   */
  static IROpcode return_to_move(IROpcode op);
};

} // namespace cfg
