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
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSetAbstractDomain.h"

namespace reaching_defs {

using reg_t = ir_analyzer::register_t;

class Domain final : public sparta::AbstractDomainReverseAdaptor<
                         sparta::PatriciaTreeSetAbstractDomain<IRInstruction*>,
                         Domain> {
 public:
  using AbstractDomainReverseAdaptor::AbstractDomainReverseAdaptor;

  // Some older compilers complain that the class is not default constructible.
  // We intended to use the default constructors of the base class (via using
  // AbstractDomainReverseAdaptor::AbstractDomainReverseAdaptor), but some
  // compilers fail to catch this. So we insert a redundant '= default'.
  Domain() = default;

  size_t size() const { return unwrap().size(); }

  const sparta::PatriciaTreeSet<IRInstruction*>& elements() const {
    return unwrap().elements();
  }
};

class Environment final
    : public sparta::AbstractDomainReverseAdaptor<
          sparta::PatriciaTreeMapAbstractEnvironment<reg_t, Domain>,
          Environment> {
 public:
  using AbstractDomainReverseAdaptor::AbstractDomainReverseAdaptor;

  Domain get(reg_t reg) { return unwrap().get(reg); }

  Environment& set(reg_t reg, const Domain& value) {
    unwrap().set(reg, value);
    return *this;
  }
};

class FixpointIterator final : public ir_analyzer::BaseIRAnalyzer<Environment> {
 public:
  explicit FixpointIterator(const cfg::ControlFlowGraph& cfg)
      : ir_analyzer::BaseIRAnalyzer<Environment>(cfg) {}

  void analyze_instruction(IRInstruction* insn,
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

  void analyze_instruction(IRInstruction* insn,
                           Environment* current_state) const override {
    if (is_move(insn->opcode())) {
      current_state->set(insn->dest(), current_state->get(insn->src(0)));
    } else if (insn->has_dest()) {
      current_state->set(insn->dest(),
                         Domain(const_cast<IRInstruction*>(insn)));
    }
  }
};

} // namespace reaching_defs
