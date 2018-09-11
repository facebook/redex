/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "AbstractDomain.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "MonotonicFixpointIterator.h"
#include "PatriciaTreeMapAbstractEnvironment.h"
#include "PatriciaTreeSetAbstractDomain.h"

namespace reaching_defs {

using reg_t = uint16_t;

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

class FixpointIterator final
    : public sparta::MonotonicFixpointIterator<cfg::GraphInterface,
                                               Environment> {
 public:
  using NodeId = cfg::Block*;

  explicit FixpointIterator(const cfg::ControlFlowGraph& cfg)
      : MonotonicFixpointIterator(cfg, cfg.blocks().size()) {}

  void analyze_node(const NodeId& block,
                    Environment* current_state) const override {
    for (const auto& mie : InstructionIterable(block)) {
      analyze_instruction(mie.insn, current_state);
    }
  }

  void analyze_instruction(const IRInstruction* insn,
                           Environment* current_state) const {
    if (insn->dests_size()) {
      current_state->set(insn->dest(),
                         Domain(const_cast<IRInstruction*>(insn)));
    }
  }

  Environment analyze_edge(
      const EdgeId&, const Environment& entry_state_at_source) const override {
    return entry_state_at_source;
  }
};

} // namespace reaching_defs
