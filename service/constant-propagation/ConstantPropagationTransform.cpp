/*
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
                                   const IRList::iterator& it) {
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

/*
 * Add an const after load param section for a known value load_param.
 * This will depend on future run of RemoveUnusedArgs pass to get the win of
 * removing not used arguments.
 */
void Transform::generate_const_param(const ConstantEnvironment& env,
                                     const IRList::iterator& it) {
  auto* insn = it->insn;
  auto value = env.get(insn->dest());
  auto replacement =
      ConstantValue::apply_visitor(value_to_instruction_visitor(insn), value);
  if (replacement.size() == 0) {
    return;
  }
  m_added_param_values.insert(m_added_param_values.end(), replacement.begin(),
                              replacement.end());
  ++m_stats.added_param_const;
}

bool Transform::eliminate_redundant_put(const ConstantEnvironment& env,
                                        const WholeProgramState& wps,
                                        const IRList::iterator& it) {
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
      TRACE(FINALINLINE, 2, "%s has %s", SHOW(field), SHOW(existing_val));
      // This field must already hold this value. We don't need to write to it
      // again.
      m_deletes.push_back(it);
      return true;
    }
    break;
  }
  default: {
    break;
  }
  }
  return false;
}

void Transform::simplify_instruction(const ConstantEnvironment& env,
                                     const WholeProgramState& wps,
                                     const IRList::iterator& it) {
  auto* insn = it->insn;
  switch (insn->opcode()) {
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE: {
    generate_const_param(env, it);
    break;
  }
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
        is_rem_int_lit(op) || is_instance_of(op) || is_rem_int_or_long(op) ||
        is_div_int_or_long(op)) {
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
  case OPCODE_USHR_INT_LIT8:
  case OPCODE_ADD_INT:
  case OPCODE_SUB_INT:
  case OPCODE_MUL_INT:
  case OPCODE_AND_INT:
  case OPCODE_OR_INT:
  case OPCODE_XOR_INT:
  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG: {
    replace_with_const(env, it);
    break;
  }

  default: {
  }
  }
}

void Transform::remove_dead_switch(const ConstantEnvironment& env,
                                   cfg::ControlFlowGraph& cfg,
                                   cfg::Block* block) {

  if (!m_config.remove_dead_switch) {
    return;
  }

  // TODO: The cfg for constant propagation is assumed to be non-editable.
  // Once the editable cfg is used, the following optimization logic should be
  // simpler.
  if (cfg.editable()) {
    return;
  }

  auto insn_it = block->get_last_insn();
  always_assert(insn_it != block->end());
  auto* insn = insn_it->insn;
  always_assert(is_switch(insn->opcode()));

  // Find successor blocks and a default block for switch
  std::unordered_set<cfg::Block*> succs;
  cfg::Block* def_block = nullptr;
  for (auto& edge : block->succs()) {
    auto type = edge->type();
    auto target = edge->target();
    if (type == cfg::EDGE_GOTO) {
      always_assert(def_block == nullptr);
      def_block = target;
    } else {
      always_assert(type == cfg::EDGE_BRANCH);
    }
    succs.insert(edge->target());
  }
  always_assert(def_block != nullptr);

  auto is_switch_label = [=](MethodItemEntry& mie) {
    return (mie.type == MFLOW_TARGET && mie.target->type == BRANCH_MULTI &&
            mie.target->src->insn == insn);
  };

  // Find a non-default block which is uniquely reachable with a constant.
  cfg::Block* reachable = nullptr;
  auto eval_switch = env.get<SignedConstantDomain>(insn->src(0));
  // If switch value is not an exact constant, do not replace the switch by a
  // goto.
  bool should_goto = eval_switch.constant_domain().is_value();
  for (auto succ : succs) {
    for (auto& mie : *succ) {
      if (is_switch_label(mie)) {
        auto eval_case =
            eval_switch.meet(SignedConstantDomain(mie.target->case_key));
        if (eval_case.is_bottom() || def_block == succ) {
          // Unreachable label or any switch targeted label in default block is
          // simply removed.
          mie.type = MFLOW_FALLTHROUGH;
          delete mie.target;
        } else {
          if (reachable != nullptr) {
            should_goto = false;
          } else {
            reachable = succ;
          }
        }
      }
    }
  }

  // When non-default blocks are unreachable, simply remove the switch.
  if (reachable == nullptr) {
    m_deletes.emplace_back(insn_it);
    ++m_stats.branches_removed;
    return;
  }

  if (!should_goto) {
    return;
  }
  ++m_stats.branches_removed;

  // Replace the switch by a goto to the uniquely reachable block
  m_replacements.push_back({insn, {new IRInstruction(OPCODE_GOTO)}});
  // Change the first label for the goto.
  bool has_changed = false;
  for (auto& mie : *reachable) {
    if (is_switch_label(mie)) {
      if (!has_changed) {
        mie.target->type = BRANCH_SIMPLE;
        has_changed = true;
      } else {
        // From the second targets, just become a nop, if any.
        mie.type = MFLOW_FALLTHROUGH;
        delete mie.target;
      }
    }
  }
  always_assert(has_changed);
}

/*
 * If the last instruction in a basic block is an if-* instruction, determine
 * whether it is dead (i.e. whether the branch always taken or never taken).
 * If it is, we can replace it with either a nop or a goto.
 */
void Transform::eliminate_dead_branch(
    const intraprocedural::FixpointIterator& intra_cp,
    const ConstantEnvironment& env,
    cfg::ControlFlowGraph& cfg,
    cfg::Block* block) {
  auto insn_it = block->get_last_insn();
  if (insn_it == block->end()) {
    return;
  }
  auto* insn = insn_it->insn;
  if (is_switch(insn->opcode())) {
    remove_dead_switch(env, cfg, block);
    return;
  }

  if (!is_conditional_branch(insn->opcode())) {
    return;
  }

  const auto& succs = cfg.get_succ_edges_if(
      block, [](const cfg::Edge* e) { return e->type() != cfg::EDGE_GHOST; });
  always_assert_log(succs.size() == 2, "actually %d\n%s", succs.size(),
                    SHOW(InstructionIterable(*block)));
  for (auto& edge : succs) {
    // Check if the fixpoint analysis has determined the successors to be
    // unreachable
    if (intra_cp.analyze_edge(edge, env).is_bottom()) {
      auto is_fallthrough = edge->type() == cfg::EDGE_GOTO;
      TRACE(CONSTP, 2, "Changed conditional branch %s as it is always %s",
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

bool Transform::replace_with_throw(const ConstantEnvironment& env,
                                   const IRList::iterator& it,
                                   IRCode* code,
                                   boost::optional<int32_t>* temp_reg) {
  auto* insn = it->insn;
  auto opcode = insn->opcode();
  size_t src_index;
  switch (opcode) {
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_AGET:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_WIDE:
  case OPCODE_AGET_SHORT:
  case OPCODE_AGET_OBJECT:
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_IGET:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_WIDE:
  case OPCODE_IGET_SHORT:
  case OPCODE_IGET_OBJECT:
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_ARRAY_LENGTH:
  case OPCODE_FILL_ARRAY_DATA:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_INTERFACE:
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_DIRECT:
    src_index = 0;
    break;
  case OPCODE_APUT:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_SHORT:
  case OPCODE_APUT_OBJECT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_IPUT:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_SHORT:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_BOOLEAN:
    src_index = 1;
    break;
  default: {
    return false;
  }
  }

  auto reg = insn->src(src_index);
  auto value = env.get(reg).maybe_get<SignedConstantDomain>();
  std::vector<IRInstruction*> new_insns;
  if (!value || !value->get_constant() || *value->get_constant() != 0) {
    return false;
  }

  // We'll replace this instruction with
  //   const tmp, 0
  //   throw tmp
  // We do not reuse reg, even if it might be null, as we need a value that is
  // throwable.
  if (!*temp_reg) {
    *temp_reg = code->allocate_temp();
  }

  IRInstruction* const_insn = new IRInstruction(OPCODE_CONST);
  const_insn->set_dest(**temp_reg)->set_literal(0);
  new_insns.emplace_back(const_insn);

  IRInstruction* throw_insn = new IRInstruction(OPCODE_THROW);
  throw_insn->set_src(0, **temp_reg);
  new_insns.emplace_back(throw_insn);
  m_replacements.emplace_back(insn, new_insns);
  m_rebuild_cfg = true;
  ++m_stats.throws;

  if (insn->has_move_result_any()) {
    auto move_result_it = std::next(it);
    if (opcode::is_move_result_any(move_result_it->insn->opcode())) {
      m_redundant_move_results.insert(move_result_it->insn);
    }
  }
  return true;
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
  for (const auto& it : m_deletes) {
    TRACE(CONSTP, 4, "Removing instruction %s", SHOW(it->insn));
    code->remove_opcode(it);
  }
  auto params = code->get_param_instructions();
  for (auto insn : m_added_param_values) {
    code->insert_before(params.end(), insn);
  }
  if (m_rebuild_cfg) {
    code->build_cfg(/* editable */);
    code->clear_cfg();
  }
}

Transform::Stats Transform::apply(
    const intraprocedural::FixpointIterator& intra_cp,
    const WholeProgramState& wps,
    IRCode* code) {
  auto& cfg = code->cfg();
  boost::optional<int32_t> temp_reg;
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    // This block is unreachable, no point mutating its instructions -- DCE
    // will be removing it anyway
    if (env.is_bottom()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      auto it = code->iterator_to(mie);
      bool any_changes = eliminate_redundant_put(env, wps, it) ||
                         replace_with_throw(env, it, code, &temp_reg);
      intra_cp.analyze_instruction(mie.insn, &env);
      if (!any_changes && !m_redundant_move_results.count(mie.insn)) {
        simplify_instruction(env, wps, code->iterator_to(mie));
      }
    }
    eliminate_dead_branch(intra_cp, env, cfg, block);
  }
  apply_changes(code);
  return m_stats;
}

} // namespace constant_propagation
