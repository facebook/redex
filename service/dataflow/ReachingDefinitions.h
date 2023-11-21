/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/AbstractDomain.h>
#include <sparta/PatriciaTreeMapAbstractEnvironment.h>
#include <sparta/PatriciaTreeSetAbstractDomain.h>

#include <utility>

#include "BaseIRAnalyzer.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IROpcode.h"

namespace reaching_defs {

using Domain = sparta::PatriciaTreeSetAbstractDomain<IRInstruction*>;

using Environment = sparta::PatriciaTreeMapAbstractEnvironment<reg_t, Domain>;

using Filter = std::function<bool(const IRInstruction*)>;

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
  Filter m_filter;

 public:
  explicit FixpointIterator(const cfg::ControlFlowGraph& cfg,
                            Filter filter = nullptr)
      : ir_analyzer::BaseIRAnalyzer<Environment>(cfg),
        m_filter(std::move(filter)) {}

  void analyze_instruction(const IRInstruction* insn,
                           Environment* current_state) const override {
    if (insn->has_dest()) {
      current_state->set(insn->dest(), make_domain(insn));
    }
  }

  bool has_filter() const { return m_filter != nullptr; }

  Domain make_domain(const IRInstruction* insn) const {
    if (!m_filter || m_filter(insn)) {
      return Domain(const_cast<IRInstruction*>(insn));
    }
    return Domain();
  }
};

class MoveAwareFixpointIterator final
    : public ir_analyzer::BaseIRAnalyzer<Environment> {
  Filter m_filter;

 public:
  explicit MoveAwareFixpointIterator(const cfg::ControlFlowGraph& cfg,
                                     Filter filter = nullptr)
      : ir_analyzer::BaseIRAnalyzer<Environment>(cfg),
        m_filter(std::move(filter)) {}

  void analyze_instruction(const IRInstruction* insn,
                           Environment* current_state) const override {
    if (opcode::is_a_move(insn->opcode())) {
      current_state->set(insn->dest(), current_state->get(insn->src(0)));
    } else if (opcode::is_move_result_any(insn->opcode())) {
      current_state->set(insn->dest(), current_state->get(RESULT_REGISTER));
      current_state->set(RESULT_REGISTER, Domain::top());
    } else if (insn->has_move_result_any()) {
      current_state->set(RESULT_REGISTER, make_domain(insn));
    } else if (insn->has_dest()) {
      current_state->set(insn->dest(), make_domain(insn));
    }
  }

  bool has_filter() const { return m_filter != nullptr; }

  Domain make_domain(const IRInstruction* insn) const {
    if (!m_filter || m_filter(insn)) {
      return Domain(const_cast<IRInstruction*>(insn));
    }
    return Domain();
  }
};

} // namespace reaching_defs
