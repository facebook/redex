/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagationTransform.h"

#include <boost/optional.hpp>

namespace constant_propagation {

// Evaluate the guard expression of an if opcode. Return boost::none if the
// branch cannot be determined to jump the same way every time. Otherwise
// return true if the branch is always taken and false if it is never taken.
boost::optional<bool> eval_if(IRInstruction*& insn,
                              const ConstantEnvironment& state) {
  if (state.is_bottom()) {
    return boost::none;
  }
  auto op = insn->opcode();
  auto scd_left = state.get(insn->src(0));
  auto scd_right =
      insn->srcs_size() > 1 ? state.get(insn->src(1)) : SignedConstantDomain(0);
  switch (op) {
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ: {
    auto cd_left = scd_left.constant_domain();
    auto cd_right = scd_right.constant_domain();
    if (!(cd_left.is_value() && cd_right.is_value())) {
      return boost::none;
    }
    if (op == OPCODE_IF_EQ || op == OPCODE_IF_EQZ) {
      return *cd_left.get_constant() == *cd_right.get_constant();
    } else { // IF_NE / IF_NEZ
      return *cd_left.get_constant() != *cd_right.get_constant();
    }
  }
  case OPCODE_IF_LE:
  case OPCODE_IF_LEZ: {
    if (scd_left.max_element() <= scd_right.min_element()) {
      return true;
    } else if (scd_left.min_element() > scd_right.max_element()) {
      return false;
    } else {
      return boost::none;
    }
  }
  case OPCODE_IF_LT:
  case OPCODE_IF_LTZ: {
    if (scd_left.max_element() < scd_right.min_element()) {
      return true;
    } else if (scd_left.min_element() >= scd_right.max_element()) {
      return false;
    } else {
      return boost::none;
    }
  }
  case OPCODE_IF_GE:
  case OPCODE_IF_GEZ: {
    if (scd_left.min_element() >= scd_right.max_element()) {
      return true;
    } else if (scd_left.max_element() < scd_right.min_element()) {
      return false;
    } else {
      return boost::none;
    }
  }
  case OPCODE_IF_GT:
  case OPCODE_IF_GTZ: {
    if (scd_left.min_element() > scd_right.max_element()) {
      return true;
    } else if (scd_left.max_element() <= scd_right.min_element()) {
      return false;
    } else {
      return boost::none;
    }
  }
  default:
    always_assert_log(false, "opcode %s must be an if", SHOW(op));
  }
}

// If we can prove the operands of a branch instruction are constant values
// replace the conditional branch with an unconditional one.
void Transform::simplify_branch(IRInstruction* insn,
                                const ConstantEnvironment& env) {
  auto constant_branch = eval_if(insn, env);

  if (!constant_branch) {
    return;
  }
  TRACE(CONSTP,
        2,
        "Changed conditional branch %s as it is always %s\n",
        SHOW(insn),
        *constant_branch ? "true" : "false");
  ++m_stats.branches_removed;
  // Transform keeps track of the target and selects the right size
  // instruction based on the offset
  m_insn_replacements.emplace_back(
      insn, new IRInstruction(*constant_branch ? OPCODE_GOTO : OPCODE_NOP));
}

// Replace an instruction that has a single destination register with a `const`
// load. `env` holds the state of the registers after `insn` has been
// evaluated. So, `env[dst]` holds the _new_ value of the destination
// register
void Transform::simplify_non_branch(IRInstruction* insn,
                                    const ConstantEnvironment& env,
                                    bool is_wide) {
  // we read from `dest` because analyze has put the new value of dst there
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
      simplify_non_branch(insn, env, false /* is_wide */);
    }
    break;
  case OPCODE_MOVE_WIDE:
    if (m_config.replace_moves_with_consts) {
      simplify_non_branch(insn, env, true /* is_wide */);
    }
    break;
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ: {
    simplify_branch(insn, env);
    break;
  }

  case OPCODE_ADD_INT_LIT16:
  case OPCODE_ADD_INT_LIT8: {
    simplify_non_branch(insn, env, false);
    break;
  }

  default: {}
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
      TRACE(CONSTP,
            4,
            "Replacing instruction %s -> %s\n",
            SHOW(old_op),
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
    auto state = intra_cp.get_entry_state_at(block);
    for (auto& mie : InstructionIterable(block)) {
      intra_cp.analyze_instruction(mie.insn, &state);
      simplify_instruction(mie.insn, state);
    }
  }
  apply_changes(code);
  return m_stats;
}

} // namespace constant_propagation
