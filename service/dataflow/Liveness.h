/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "BaseIRAnalyzer.h"
#include "ControlFlow.h"
#include "PatriciaTreeSetAbstractDomain.h"

using LivenessDomain = sparta::PatriciaTreeSetAbstractDomain<reg_t>;

class LivenessFixpointIterator final
    : public ir_analyzer::BaseBackwardsIRAnalyzer<LivenessDomain> {
 public:
  explicit LivenessFixpointIterator(const cfg::ControlFlowGraph& cfg)
      : ir_analyzer::BaseBackwardsIRAnalyzer<LivenessDomain>(cfg) {}

  void analyze_instruction(IRInstruction* insn,
                           LivenessDomain* current_state) const override {
    if (insn->has_dest()) {
      current_state->remove(insn->dest());
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      current_state->add(insn->src(i));
    }
  }

  const LivenessDomain& get_live_in_vars_at(const NodeId& block) const {
    return get_exit_state_at(block);
  }

  const LivenessDomain& get_live_out_vars_at(const NodeId& block) const {
    return get_entry_state_at(block);
  }
};
