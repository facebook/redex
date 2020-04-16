/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "AbstractDomain.h"
#include "BaseIRAnalyzer.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IROpcode.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSetAbstractDomain.h"

namespace reaching_defs {

using Domain = sparta::PatriciaTreeSetAbstractDomain<IRInstruction*>;

using Environment = sparta::PatriciaTreeMapAbstractEnvironment<reg_t, Domain>;

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  explicit FixpointIterator(const cfg::ControlFlowGraph& cfg)
      : ir_analyzer::BaseIRAnalyzer<Environment>(cfg) {}

  void analyze_instruction(const IRInstruction* insn,
                           Environment* current_state) const override {
    if (insn->has_dest()) {
      current_state->set(insn->dest(),
                         Domain(const_cast<IRInstruction*>(insn)));
    }
  }
};

class MoveAwareFixpointIterator final
    : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  explicit MoveAwareFixpointIterator(const cfg::ControlFlowGraph& cfg)
      : ir_analyzer::BaseIRAnalyzer<Environment>(cfg) {}

  void analyze_instruction(const IRInstruction* insn,
                           Environment* current_state) const override {
    constexpr reg_t RESULT = ir_analyzer::RESULT_REGISTER;
    if (is_move(insn->opcode())) {
      current_state->set(insn->dest(), current_state->get(insn->src(0)));
    } else if (opcode::is_move_result_any(insn->opcode())) {
      current_state->set(insn->dest(), current_state->get(RESULT));
      current_state->set(RESULT, Domain::top());
    } else if (insn->has_move_result_any()) {
      current_state->set(RESULT, Domain(const_cast<IRInstruction*>(insn)));
    } else if (insn->has_dest()) {
      current_state->set(insn->dest(),
                         Domain(const_cast<IRInstruction*>(insn)));
    }
  }
};

} // namespace reaching_defs
