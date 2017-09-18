/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "ControlFlow.h"
#include "FixpointIterators.h"
#include "SparseSetAbstractDomain.h"

namespace regalloc {

using namespace std::placeholders;
using LivenessDomain = SparseSetAbstractDomain;

class LivenessFixpointIterator final
    : public MonotonicFixpointIterator<Block*, LivenessDomain> {
 public:
  using NodeId = Block*;

  LivenessFixpointIterator(NodeId exit_block)
      : MonotonicFixpointIterator(exit_block,
                                  std::bind(&Block::preds, _1),
                                  std::bind(&Block::succs, _1)) {}

  void analyze_node(const NodeId& block,
                    LivenessDomain* current_state) const override {
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type == MFLOW_OPCODE) {
        analyze_instruction(it->insn, current_state);
      }
    }
  }

  LivenessDomain analyze_edge(
      const NodeId& /* source_block */,
      const NodeId& /* target_block */,
      const LivenessDomain& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  void analyze_instruction(const IRInstruction* insn,
                           LivenessDomain* current_state) const {
    if (insn->dests_size()) {
      current_state->remove(insn->dest());
    }
    for (size_t i = 0; i < insn->srcs_size(); ++i) {
      current_state->add(insn->src(i));
    }
    always_assert(!opcode::has_range(insn->opcode()));
  }

  LivenessDomain get_live_in_vars_at(const NodeId& block) const {
    return get_exit_state_at(block);
  }

  LivenessDomain get_live_out_vars_at(const NodeId& block) const {
    return get_entry_state_at(block);
  }
};

} // namespace regalloc
