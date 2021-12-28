/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <utility>

#include "ControlFlow.h"
#include "DexUtil.h"
#include "IROpcode.h"
#include "InitClassDomain.h"
#include "InitClassesWithSideEffects.h"
#include "MonotonicFixpointIterator.h"
#include "Resolver.h"

namespace init_classes {

class InitClassForwardFixpointIterator final
    : public sparta::MonotonicFixpointIterator<cfg::GraphInterface,
                                               InitClassDomain> {
 private:
  const InitClassesWithSideEffects& m_init_classes_with_side_effects;

 public:
  explicit InitClassForwardFixpointIterator(
      const InitClassesWithSideEffects& init_classes_with_side_effects,
      const cfg::ControlFlowGraph& cfg)
      : MonotonicFixpointIterator(cfg),
        m_init_classes_with_side_effects(init_classes_with_side_effects) {}

  InitClassDomain initial_env(const DexType* declaring_type) {
    InitClassDomain env;
    env.insert(m_init_classes_with_side_effects, declaring_type);
    return env;
  }

  void analyze_instruction(const IRInstruction* insn,
                           InitClassDomain* current_state) const {
    auto init_class = ::get_init_class_type_demand(insn);
    current_state->insert(m_init_classes_with_side_effects, init_class);
  }

  void analyze_instruction_no_throw(const IRInstruction* insn,
                                    InitClassDomain* current_state) const {
    auto op = insn->opcode();
    if (opcode::is_an_ifield_op(op)) {
      auto field = resolve_field(insn->get_field(), FieldSearch::Instance);
      if (field) {
        current_state->insert(m_init_classes_with_side_effects,
                              field->get_class());
      }
    } else if (opcode::is_invoke_virtual(op)) {
      auto method = resolve_method(insn->get_method(), MethodSearch::Virtual);
      if (method) {
        current_state->insert(m_init_classes_with_side_effects,
                              method->get_class());
      }
    }
  }

  void analyze_instruction(const IRInstruction* insn,
                           InitClassDomain* current_state,
                           bool is_last) const {
    analyze_instruction(insn, current_state);
    if (!is_last) {
      analyze_instruction_no_throw(insn, current_state);
    }
  }

  void analyze_node(const NodeId& block,
                    InitClassDomain* state_at_entry) const override {
    auto last_insn = block->get_last_insn();
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      analyze_instruction(insn, state_at_entry, insn == last_insn->insn);
    }
  }

  InitClassDomain analyze_edge(
      const cfg::GraphInterface::EdgeId& edge,
      const InitClassDomain& exit_state_at_source) const override {
    if (edge->type() == cfg::EDGE_THROW) {
      return exit_state_at_source;
    }

    auto last_insn_it = edge->src()->get_last_insn();
    if (last_insn_it == edge->src()->end()) {
      return exit_state_at_source;
    }

    auto current_state = exit_state_at_source;
    analyze_instruction_no_throw(last_insn_it->insn, &current_state);
    return current_state;
  }
};

} // namespace init_classes
