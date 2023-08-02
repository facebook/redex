/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/ConstantAbstractDomain.h>
#include <sparta/PatriciaTreeMapAbstractEnvironment.h>

#include "BaseIRAnalyzer.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IROpcode.h"
#include "MethodUtil.h"

namespace reaching_initializeds {

using Domain = sparta::ConstantAbstractDomain<bool>;

/**
 * For each register, whether it represents an initialized value.
 **/
using Environment = sparta::PatriciaTreeMapAbstractEnvironment<reg_t, Domain>;

enum class Mode {
  // only track the initialized-state of the first parameter
  FirstLoadParam,
  // only track the initialized-state of new-instance results.
  NewInstances,
};

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  FixpointIterator(const cfg::ControlFlowGraph& cfg, Mode mode)
      : ir_analyzer::BaseIRAnalyzer<Environment>(cfg),
        m_first_init_load_param_insn(
            mode == Mode::FirstLoadParam
                ? cfg.get_param_instructions().begin()->insn
                : nullptr) {}

  void analyze_instruction(const IRInstruction* insn,
                           Environment* current_state) const override {
    auto opcode = insn->opcode();
    if (opcode == OPCODE_INVOKE_DIRECT && method::is_init(insn->get_method())) {
      current_state->set(insn->src(0), Domain(true));
    } else if (insn == m_first_init_load_param_insn) {
      current_state->set(insn->dest(), Domain(false));
    } else if (opcode::is_a_move(opcode)) {
      current_state->set(insn->dest(), current_state->get(insn->src(0)));
    } else if (opcode::is_move_result_any(insn->opcode())) {
      current_state->set(insn->dest(), current_state->get(RESULT_REGISTER));
      current_state->set(RESULT_REGISTER, Domain::top());
    } else if (insn->has_move_result_any()) {
      const auto value = Domain(!!m_first_init_load_param_insn ||
                                opcode != OPCODE_NEW_INSTANCE);
      current_state->set(RESULT_REGISTER, value);
    } else if (insn->has_dest()) {
      current_state->set(insn->dest(), Domain(true));
    }
  }

 private:
  IRInstruction* m_first_init_load_param_insn;
};

using ReachingInitializedsEnvironments =
    std::unordered_map<const IRInstruction*,
                       reaching_initializeds::Environment>;

inline ReachingInitializedsEnvironments get_reaching_initializeds(
    cfg::ControlFlowGraph& cfg, Mode mode) {
  reaching_initializeds::FixpointIterator fp_iter(cfg, mode);
  fp_iter.run({});
  ReachingInitializedsEnvironments res;
  for (auto block : cfg.blocks()) {
    auto env = fp_iter.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      res[mie.insn] = env;
      fp_iter.analyze_instruction(mie.insn, &env);
    }
  }
  return res;
}

} // namespace reaching_initializeds
