/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagationTransform.h"

#include "Transform.h"

namespace constant_propagation {

/*
 * Replace an instruction that has a single destination register with a `const`
 * load. `env` holds the state of the registers after `insn` has been
 * evaluated. So, `env.get(dest)` holds the _new_ value of the destination
 * register.
 */
void Transform::replace_with_const(IRInstruction* insn,
                                   const ConstantEnvironment& env,
                                   bool is_wide) {
  auto cst = env.get(insn->dest()).constant_domain().get_constant();
  if (!cst) {
    return;
  }
  IRInstruction* replacement =
      new IRInstruction(is_wide ? OPCODE_CONST_WIDE : OPCODE_CONST);
  replacement->set_literal(*cst);
  replacement->set_dest(insn->dest());

  TRACE(CONSTP, 5, "Replacing %s with %s\n", SHOW(insn), SHOW(replacement));
  m_insn_replacements.emplace_back(insn, replacement);
  ++m_stats.materialized_consts;
}

void Transform::simplify_instruction(IRInstruction* insn,
                                     const ConstantEnvironment& env) {
  switch (insn->opcode()) {
  case OPCODE_MOVE:
    if (m_config.replace_moves_with_consts) {
      replace_with_const(insn, env, false /* is_wide */);
    }
    break;
  case OPCODE_MOVE_WIDE:
    if (m_config.replace_moves_with_consts) {
      replace_with_const(insn, env, true /* is_wide */);
    }
    break;
  case OPCODE_ADD_INT_LIT16:
  case OPCODE_ADD_INT_LIT8: {
    replace_with_const(insn, env, false /* is_wide */);
    break;
  }

  default: {}
  }
}

/*
 * If the last instruction in a basic block is an if-* instruction, determine
 * whether it is dead (i.e. whether the branch always taken or never taken).
 * If it is, we can replace it with either a nop or a goto.
 */
void Transform::eliminate_dead_branch(
    const intraprocedural::FixpointIterator& intra_cp,
    Block* block,
    const ConstantEnvironment& env) {
  auto insn_it = transform::find_last_instruction(block);
  if (insn_it == block->end()) {
    return;
  }
  auto* insn = insn_it->insn;
  if (!is_conditional_branch(insn->opcode())) {
    return;
  }
  always_assert(block->succs().size() == 2);
  for (auto& edge : block->succs()) {
    // Check if the fixpoint analysis has determined the successors to be
    // unreachable
    if (intra_cp.analyze_edge(edge, env).is_bottom()) {
      auto is_fallthrough = edge->type() == EDGE_GOTO;
      TRACE(CONSTP, 2, "Changed conditional branch %s as it is always %s\n",
            SHOW(insn), is_fallthrough ? "true" : "false");
      ++m_stats.branches_removed;
      m_insn_replacements.emplace_back(
          insn, new IRInstruction(is_fallthrough ? OPCODE_GOTO : OPCODE_NOP));
      // Assuming :block is reachable, then at least one of its successors must
      // be reachable, so we can break after finding one that's unreachable
      break;
    }
  }
}

void Transform::apply_changes(IRCode* code) {
  for (auto const& p : m_insn_replacements) {
    IRInstruction* old_op = p.first;
    IRInstruction* new_op = p.second;
    if (new_op->opcode() == OPCODE_NOP) {
      TRACE(CONSTP, 4, "Removing instruction %s\n", SHOW(old_op));
      code->remove_opcode(old_op);
      delete new_op;
    } else {
      TRACE(CONSTP, 4, "Replacing instruction %s -> %s\n", SHOW(old_op),
            SHOW(new_op));
      if (is_branch(old_op->opcode())) {
        code->replace_branch(old_op, new_op);
      } else {
        code->replace_opcode(old_op, new_op);
      }
    }
  }
}

Transform::Stats Transform::apply(
    const intraprocedural::FixpointIterator& intra_cp, IRCode* code) {
  auto& cfg = code->cfg();
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    // This block is unreachable, no point mutating its instructions -- DCE
    // will be removing it anyway
    if (env.is_bottom()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      intra_cp.analyze_instruction(mie.insn, &env);
      simplify_instruction(mie.insn, env);
    }
    eliminate_dead_branch(intra_cp, block, env);
  }
  apply_changes(code);
  return m_stats;
}

} // namespace constant_propagation
