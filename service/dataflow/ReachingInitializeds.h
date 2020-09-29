/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "BaseIRAnalyzer.h"
#include "ConstantAbstractDomain.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IROpcode.h"
#include "MethodUtil.h"
#include "PatriciaTreeMapAbstractEnvironment.h"

namespace reaching_initializeds {

using Domain = sparta::ConstantAbstractDomain<bool>;

/**
 * For each register, whether it represents an initialized value.
 **/
using Environment = sparta::PatriciaTreeMapAbstractEnvironment<reg_t, Domain>;

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  FixpointIterator(const cfg::ControlFlowGraph& cfg, bool is_init)
      : ir_analyzer::BaseIRAnalyzer<Environment>(cfg),
        m_first_init_load_param_insn(
            is_init ? cfg.get_param_instructions().begin()->insn : nullptr) {}

  void analyze_instruction(const IRInstruction* insn,
                           Environment* current_state) const override {
    auto opcode = insn->opcode();
    if (opcode == OPCODE_INVOKE_DIRECT && method::is_init(insn->get_method())) {
      current_state->set(insn->src(0), Domain(true));
    } else if (insn == m_first_init_load_param_insn) {
      current_state->set(insn->dest(), Domain(false));
    } else if (opcode::is_move(opcode)) {
      current_state->set(insn->dest(), current_state->get(insn->src(0)));
    } else if (opcode::is_move_result_any(insn->opcode())) {
      current_state->set(insn->dest(), current_state->get(RESULT_REGISTER));
      current_state->set(RESULT_REGISTER, Domain::top());
    } else if (insn->has_move_result_any()) {
      const auto value = Domain(opcode != OPCODE_NEW_INSTANCE);
      current_state->set(RESULT_REGISTER, value);
    } else if (insn->has_dest()) {
      current_state->set(insn->dest(), Domain(true));
    }
  }

 private:
  IRInstruction* m_first_init_load_param_insn;
};

} // namespace reaching_initializeds
