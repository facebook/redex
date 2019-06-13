/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
void Transform::replace_with_const(const ConstantEnvironment& env,
                                   IRList::iterator it) {
  auto* insn = it->insn;
  auto value = env.get(insn->dest());
  auto replacement =
      ConstantValue::apply_visitor(value_to_instruction_visitor(insn), value);
  if (replacement.size() == 0) {
    return;
  }
  if (opcode::is_move_result_pseudo(insn->opcode())) {
    m_replacements.emplace_back(std::prev(it)->insn, replacement);
  } else {
    m_replacements.emplace_back(insn, replacement);
  }
  ++m_stats.materialized_consts;
}

void Transform::eliminate_redundant_put(const ConstantEnvironment& env,
                                        const WholeProgramState& wps,
                                        IRList::iterator it) {
  auto* insn = it->insn;
  switch (insn->opcode()) {
  case OPCODE_SPUT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_SPUT_SHORT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_IPUT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_SHORT:
  case OPCODE_IPUT_WIDE: {
    auto* field = resolve_field(insn->get_field());
    if (!field) {
      break;
    }
    // WholeProgramState tells us the abstract value of a field across
    // all program traces outside their class's <clinit> or <init>; the
    // ConstantEnvironment tells us the abstract value
    // of a non-escaping field at this particular program point.
    auto existing_val = m_config.class_under_init == field->get_class()
                            ? env.get(field)
                            : wps.get_field_value(field);
    auto new_val = env.get(insn->src(0));
    if (ConstantValue::apply_visitor(runtime_equals_visitor(), existing_val,
                                     new_val)) {
      TRACE(FINALINLINE, 2, "%s has %s\n", SHOW(field), SHOW(existing_val));
      // This field must already hold this value. We don't need to write to it
      // again.
      m_deletes.push_back(it);
    }
    break;
  }
  default: {}
  }
}

void Transform::simplify_instruction(const ConstantEnvironment& env,
                                     const WholeProgramState& wps,
                                     IRList::iterator it) {
  auto* insn = it->insn;
  switch (insn->opcode()) {
  case OPCODE_MOVE:
  case OPCODE_MOVE_WIDE:
    if (m_config.replace_moves_with_consts) {
      replace_with_const(env, it);
    }
    break;
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT: {
    auto* primary_insn = ir_list::primary_instruction_of_move_result_pseudo(it);
    auto op = primary_insn->opcode();
    if (is_sget(op) || is_iget(op) || is_aget(op) || is_div_int_lit(op) ||
        is_rem_int_lit(op)) {
      replace_with_const(env, it);
    }
    break;
  }
  // We currently don't replace move-result opcodes with consts because it's
  // unlikely that we can get a more compact encoding (move-result can address
  // 8-bit register operands while taking up just 1 code unit). However it can
  // be a net win if we can remove the invoke opcodes as well -- we need a
  // purity analysis for that though.
  /*
  case OPCODE_MOVE_RESULT:
  case OPCODE_MOVE_RESULT_WIDE:
  case OPCODE_MOVE_RESULT_OBJECT: {
    replace_with_const(it, env);
    break;
  }
  */
  case OPCODE_ADD_INT_LIT16:
  case OPCODE_ADD_INT_LIT8:
  case OPCODE_RSUB_INT:
  case OPCODE_RSUB_INT_LIT8:
  case OPCODE_MUL_INT_LIT16:
  case OPCODE_MUL_INT_LIT8:
  case OPCODE_AND_INT_LIT16:
  case OPCODE_AND_INT_LIT8:
  case OPCODE_OR_INT_LIT16:
  case OPCODE_OR_INT_LIT8:
  case OPCODE_XOR_INT_LIT16:
  case OPCODE_XOR_INT_LIT8:
  case OPCODE_SHL_INT_LIT8:
  case OPCODE_SHR_INT_LIT8:
  case OPCODE_USHR_INT_LIT8: {
    replace_with_const(env, it);
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
    const ConstantEnvironment& env,
    cfg::Block* block) {
  auto insn_it = block->get_last_insn();
  if (insn_it == block->end()) {
    return;
  }
  auto* insn = insn_it->insn;
  if (!is_conditional_branch(insn->opcode())) {
    return;
  }
  always_assert_log(block->succs().size() == 2, "actually %d\n%s",
                    block->succs().size(), SHOW(InstructionIterable(*block)));
  for (auto& edge : block->succs()) {
    // Check if the fixpoint analysis has determined the successors to be
    // unreachable
    if (intra_cp.analyze_edge(edge, env).is_bottom()) {
      auto is_fallthrough = edge->type() == cfg::EDGE_GOTO;
      TRACE(CONSTP, 2, "Changed conditional branch %s as it is always %s\n",
            SHOW(insn), is_fallthrough ? "true" : "false");
      ++m_stats.branches_removed;
      if (is_fallthrough) {
        m_replacements.push_back({insn, {new IRInstruction(OPCODE_GOTO)}});
      } else {
        m_deletes.emplace_back(insn_it);
      }
      // Assuming :block is reachable, then at least one of its successors must
      // be reachable, so we can break after finding one that's unreachable
      break;
    }
  }
}

void Transform::apply_changes(IRCode* code) {
  for (auto const& p : m_replacements) {
    IRInstruction* old_op = p.first;
    std::vector<IRInstruction*> new_ops = p.second;
    if (is_branch(old_op->opcode())) {
      always_assert(new_ops.size() == 1);
      code->replace_branch(old_op, new_ops.at(0));
    } else {
      code->replace_opcode(old_op, new_ops);
    }
  }
  for (auto it : m_deletes) {
    TRACE(CONSTP, 4, "Removing instruction %s\n", SHOW(it->insn));
    code->remove_opcode(it);
  }
}

Transform::Stats Transform::apply(
    const intraprocedural::FixpointIterator& intra_cp,
    const WholeProgramState& wps,
    IRCode* code) {
  auto& cfg = code->cfg();
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    // This block is unreachable, no point mutating its instructions -- DCE
    // will be removing it anyway
    if (env.is_bottom()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      eliminate_redundant_put(env, wps, code->iterator_to(mie));
      intra_cp.analyze_instruction(mie.insn, &env);
      simplify_instruction(env, wps, code->iterator_to(mie));
    }
    eliminate_dead_branch(intra_cp, env, block);
  }
  apply_changes(code);
  return m_stats;
}

} // namespace constant_propagation
