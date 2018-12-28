/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "IRInstruction.h"
#include "MonotonicFixpointIterator.h"

namespace ir_analyzer {

using register_t = uint32_t;

// We use this special register to denote the result of a method invocation or a
// filled-array creation. If the result is a wide value, RESULT_REGISTER + 1
// holds the second component of the result.
constexpr register_t RESULT_REGISTER =
    std::numeric_limits<register_t>::max() - 1;

template <typename Domain>
class IRInstructionAnalyzer
    : public sparta::MonotonicFixpointIterator<cfg::GraphInterface, Domain> {
 public:
  using NodeId = cfg::Block*;
  using Environment = Domain;

  explicit IRInstructionAnalyzer(const cfg::ControlFlowGraph& cfg)
      : sparta::MonotonicFixpointIterator<cfg::GraphInterface, Domain>(
            cfg, cfg.blocks().size()) {}

  virtual void analyze_node(const NodeId& node,
                            Domain* current_state) const override {
    for (auto& mie : InstructionIterable(node)) {
      if (mie.type == MFLOW_OPCODE) {
        analyze_instruction(mie.insn, current_state);
      }
    }
  }

  Domain analyze_edge(const cfg::GraphInterface::EdgeId&,
                      const Domain& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  virtual void analyze_instruction(IRInstruction* insn,
                                   Domain* current_state) const = 0;
};

} // namespace ir_analyzer
