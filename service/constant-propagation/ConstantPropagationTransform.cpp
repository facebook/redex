/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationTransform.h"

#include "ReachableClasses.h"
#include "ReachingDefinitions.h"
#include "RedexContext.h"
#include "ScopedMetrics.h"
#include "SignedConstantDomain.h"
#include "StlUtil.h"
#include "Trace.h"
#include "Transform.h"
#include "TypeInference.h"

namespace constant_propagation {

/*
 * Replace an instruction that has a single destination register with a `const`
 * load. `env` holds the state of the registers after `insn` has been
 * evaluated. So, `env.get(dest)` holds the _new_ value of the destination
 * register.
 */
bool Transform::replace_with_const(const ConstantEnvironment& env,
                                   const cfg::InstructionIterator& cfg_it,
                                   const XStoreRefs* xstores,
                                   const DexType* declaring_type) {
  auto* insn = cfg_it->insn;
  auto value = env.get(insn->dest());
  auto replacement = ConstantValue::apply_visitor(
      value_to_instruction_visitor(insn, xstores, declaring_type), value);
  if (replacement.empty()) {
    return false;
  }
  if (opcode::is_a_move_result_pseudo(insn->opcode())) {
    auto primary_it = cfg_it.cfg().primary_instruction_of_move_result(cfg_it);
    m_mutation->replace(primary_it, replacement);
  } else {
    m_mutation->replace(cfg_it, replacement);
  }
  ++m_stats.materialized_consts;
  return true;
}

/*
 * Add an const after load param section for a known value load_param.
 * This will depend on future run of RemoveUnusedArgs pass to get the win of
 * removing not used arguments.
 */
void Transform::generate_const_param(const ConstantEnvironment& env,
                                     const cfg::InstructionIterator& cfg_it,
                                     const XStoreRefs* xstores,
                                     const DexType* declaring_type) {
  auto* insn = cfg_it->insn;
  auto value = env.get(insn->dest());
  auto replacement = ConstantValue::apply_visitor(
      value_to_instruction_visitor(insn, xstores, declaring_type), value);
  if (replacement.empty()) {
    return;
  }
  m_added_param_values.insert(m_added_param_values.end(), replacement.begin(),
                              replacement.end());
  ++m_stats.added_param_const;
}

bool Transform::eliminate_redundant_null_check(
    const ConstantEnvironment& env,
    const WholeProgramState& /* unused */,
    const cfg::InstructionIterator& cfg_it) {
  auto* insn = cfg_it->insn;
  switch (insn->opcode()) {
  case OPCODE_INVOKE_STATIC: {
    // Kotlin null check.
    if (auto index = get_null_check_object_index(
            insn, m_runtime_cache.kotlin_null_check_assertions)) {
      ++m_stats.null_checks_method_calls;
      auto val = env.get(insn->src(*index)).maybe_get<SignedConstantDomain>();
      if (val && val->interval() == sign_domain::Interval::NEZ) {
        m_mutation->remove(cfg_it);
        ++m_stats.null_checks;
        return true;
      }
    }
    // Redex null check.
    if (insn->get_method() == m_runtime_cache.redex_null_check_assertion) {
      ++m_stats.null_checks_method_calls;
      auto val = env.get(insn->src(0)).maybe_get<SignedConstantDomain>();
      if (val && val->interval() == sign_domain::Interval::NEZ) {
        m_mutation->remove(cfg_it);
        ++m_stats.null_checks;
        return true;
      }
    }
    break;
  }
  default:
    break;
  }
  return false;
}

bool Transform::eliminate_redundant_put(
    const ConstantEnvironment& env,
    const WholeProgramState& wps,
    const cfg::InstructionIterator& cfg_it) {
  auto* insn = cfg_it->insn;
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
    // WholeProgramState tells us the observable abstract value of a field
    // across all program traces outside their class's <clinit> or <init>, so we
    // need to join with 0 here as we are effectively creating a new observation
    // point at which the field might still have its default value.
    // The ConstantEnvironment tells us the abstract value of a non-escaping
    // field at this particular program point.
    ConstantValue existing_val;
    if (m_config.class_under_init == field->get_class()) {
      existing_val = env.get(field);
    } else {
      existing_val = wps.get_field_value(field);
      existing_val.join_with(SignedConstantDomain(0));
    }
    auto new_val = env.get(insn->src(0));
    if (ConstantValue::apply_visitor(runtime_equals_visitor(), existing_val,
                                     new_val)) {
      TRACE(FINALINLINE, 2, "%s has %s", SHOW(field), SHOW(existing_val));
      // This field must already hold this value. We don't need to write to it
      // again.
      m_mutation->remove(cfg_it);
      ++m_stats.redundant_puts_removed;
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

namespace {

void try_simplify(const ConstantEnvironment& env,
                  const cfg::InstructionIterator& cfg_it,
                  const Transform::Config& config,
                  cfg::CFGMutation& mutation) {
  auto* insn = cfg_it->insn;

  auto reg_is_exact = [&env](reg_t reg, int64_t val) {
    auto value = env.get(reg).maybe_get<SignedConstantDomain>();
    if (!value || !value->get_constant() || *value->get_constant() != val) {
      return false;
    }
    return true;
  };

  auto reg_fits_lit = [&](reg_t reg) -> std::optional<int16_t> {
    auto value = env.get(reg).maybe_get<SignedConstantDomain>();
    if (!value || !value->get_constant()) {
      return std::nullopt;
    }
    int64_t val = *value->get_constant();
    if (config.to_int_lit8 && val >= -128 && val <= 127) {
      return (int16_t)val;
    }
    if (config.to_int_lit16 && val >= -32768 && val <= 32767) {
      return (int16_t)val;
    }
    return std::nullopt;
  };

  auto maybe_reduce_lit = [&](size_t idx) -> bool {
    auto val = reg_fits_lit(insn->src(idx));
    if (!val) {
      return false;
    }

    auto new_op = [&]() -> IROpcode {
      switch (insn->opcode()) {
      case OPCODE_ADD_INT:
        return OPCODE_ADD_INT_LIT;
      // TODO: SUB to RSUB
      case OPCODE_MUL_INT:
        return OPCODE_MUL_INT_LIT;
      case OPCODE_AND_INT:
        return OPCODE_AND_INT_LIT;
      case OPCODE_OR_INT:
        return OPCODE_OR_INT_LIT;
      case OPCODE_XOR_INT:
        return OPCODE_XOR_INT_LIT;
      default:
        always_assert(false);
      }
      not_reached();
    }();

    auto repl = new IRInstruction(new_op);
    repl->set_src(0, insn->src(idx == 0 ? 1 : 0));
    repl->set_dest(insn->dest());
    repl->set_literal(*val);
    mutation.replace(cfg_it, {repl});
    return true;
  };

  auto maybe_reduce_lit_both = [&]() {
    if (maybe_reduce_lit(0)) {
      return true;
    }
    if (maybe_reduce_lit(1)) {
      return true;
    }
    return false;
  };

  auto replace_with_move = [&](reg_t src_reg) {
    auto* move = new IRInstruction(OPCODE_MOVE);
    move->set_src(0, src_reg);
    move->set_dest(insn->dest());
    mutation.replace(cfg_it, {move});
  };

  auto replace_with_const = [&](int64_t val) {
    auto* c = new IRInstruction(OPCODE_CONST);
    c->set_dest(insn->dest());
    c->set_literal(val);
    mutation.replace(cfg_it, {c});
  };

  auto replace_with_neg = [&](reg_t src_reg) {
    auto* neg = new IRInstruction(OPCODE_NEG_INT);
    neg->set_src(0, src_reg);
    neg->set_dest(insn->dest());
    mutation.replace(cfg_it, {neg});
  };

  switch (insn->opcode()) {
    // These should have been handled by PeepHole, really.

  case OPCODE_ADD_INT_LIT: {
    if (insn->get_literal() == 0) {
      replace_with_move(insn->src(0));
    }
    break;
  }

  case OPCODE_RSUB_INT_LIT: {
    if (insn->get_literal() == 0) {
      replace_with_neg(insn->src(0));
    }
    break;
  }

  case OPCODE_MUL_INT_LIT: {
    if (insn->get_literal() == 1) {
      replace_with_move(insn->src(0));
      break;
    }
    if (insn->get_literal() == 0) {
      replace_with_const(0);
      break;
    }
    if (insn->get_literal() == -1) {
      replace_with_neg(insn->src(0));
      break;
    }
    break;
  }
  case OPCODE_AND_INT_LIT: {
    if (insn->get_literal() == 0) {
      replace_with_const(0);
      break;
    }
    if (insn->get_literal() == -1) {
      replace_with_move(insn->src(0));
      break;
    }
    break;
  }
  case OPCODE_OR_INT_LIT: {
    if (insn->get_literal() == 0) {
      replace_with_move(insn->src(0));
      break;
    }
    if (insn->get_literal() == -1) {
      replace_with_const(-1);
      break;
    }
    break;
  }
  case OPCODE_XOR_INT_LIT: {
    // TODO
    break;
  }

  case OPCODE_SHL_INT_LIT:
  case OPCODE_USHR_INT_LIT:
  case OPCODE_SHR_INT_LIT: {
    // Can at most simplify the operand, but doesn't make much sense.
    break;
  }

  case OPCODE_ADD_INT: {
    if (reg_is_exact(insn->src(0), 0)) {
      replace_with_move(insn->src(1));
    } else if (reg_is_exact(insn->src(1), 0)) {
      replace_with_move(insn->src(0));
    } else if (maybe_reduce_lit_both()) {
      break;
    }
    break;
  }

  case OPCODE_SUB_INT: {
    if (reg_is_exact(insn->src(0), 0)) {
      replace_with_neg(insn->src(1));
    } else if (reg_is_exact(insn->src(1), 0)) {
      replace_with_move(insn->src(0));
    }
    break;
  }

  case OPCODE_MUL_INT: {
    if (reg_is_exact(insn->src(0), 1)) {
      replace_with_move(insn->src(1));
    } else if (reg_is_exact(insn->src(1), 1)) {
      replace_with_move(insn->src(0));
    } else if (reg_is_exact(insn->src(0), 0) || reg_is_exact(insn->src(1), 0)) {
      replace_with_const(0);
    } else if (reg_is_exact(insn->src(0), -1)) {
      replace_with_neg(insn->src(1));
    } else if (reg_is_exact(insn->src(1), -1)) {
      replace_with_neg(insn->src(0));
    } else if (maybe_reduce_lit_both()) {
      break;
    }
    break;
  }

  case OPCODE_AND_INT: {
    if (reg_is_exact(insn->src(0), -1)) {
      replace_with_move(insn->src(1));
    } else if (reg_is_exact(insn->src(1), -1)) {
      replace_with_move(insn->src(0));
    } else if (reg_is_exact(insn->src(0), 0) || reg_is_exact(insn->src(1), 0)) {
      replace_with_const(0);
    } else if (maybe_reduce_lit_both()) {
      break;
    }
    break;
  }

  case OPCODE_OR_INT: {
    if (reg_is_exact(insn->src(0), 0)) {
      replace_with_move(insn->src(1));
    } else if (reg_is_exact(insn->src(1), 0)) {
      replace_with_move(insn->src(0));
    } else if (reg_is_exact(insn->src(0), -1) ||
               reg_is_exact(insn->src(1), -1)) {
      replace_with_const(-1);
    } else if (maybe_reduce_lit_both()) {
      break;
    }
    break;
  }

  case OPCODE_XOR_INT:
    if (maybe_reduce_lit_both()) {
      break;
    }
    break;

  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG:
    // TODO: More complicated version of the above.
    break;

  default:
    return;
  }
}

} // namespace

bool Transform::assumenosideeffects(DexMethodRef* ref, DexMethod* meth) const {
  if (::assumenosideeffects(meth)) {
    return true;
  }
  return m_config.pure_methods->find(ref) != m_config.pure_methods->end();
}

void Transform::simplify_instruction(const ConstantEnvironment& env,
                                     const WholeProgramState& wps,
                                     const cfg::InstructionIterator& cfg_it,
                                     const XStoreRefs* xstores,
                                     const DexType* declaring_type) {
  auto* insn = cfg_it->insn;
  switch (insn->opcode()) {
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE: {
    if (m_config.add_param_const) {
      generate_const_param(env, cfg_it, xstores, declaring_type);
    }
    break;
  }
  case OPCODE_MOVE:
  case OPCODE_MOVE_WIDE:
    if (m_config.replace_moves_with_consts) {
      replace_with_const(env, cfg_it, xstores, declaring_type);
    }
    break;
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT: {
    auto& cfg = cfg_it.cfg();
    auto primary_insn = cfg.primary_instruction_of_move_result(cfg_it)->insn;
    auto op = primary_insn->opcode();
    if (opcode::is_an_sget(op) || opcode::is_an_iget(op) ||
        opcode::is_an_aget(op) || opcode::is_div_int_lit(op) ||
        opcode::is_rem_int_lit(op) || opcode::is_instance_of(op) ||
        opcode::is_rem_int_or_long(op) || opcode::is_div_int_or_long(op) ||
        opcode::is_check_cast(op)) {
      replace_with_const(env, cfg_it, xstores, declaring_type);
    }
    break;
  }
  // Currently it's default to not replace move-result opcodes with consts
  // because it's unlikely that we can get a more compact encoding (move-result
  // can address 8-bit register operands while taking up just 1 code unit).
  // However it can be a net win if we can remove the invoke opcodes as well --
  // we need a purity analysis for that though.
  case OPCODE_MOVE_RESULT:
  case OPCODE_MOVE_RESULT_WIDE:
  case OPCODE_MOVE_RESULT_OBJECT: {
    if (m_config.replace_move_result_with_consts) {
      replace_with_const(env, cfg_it, xstores, declaring_type);
      break;
    }
    if (!m_config.getter_methods_for_immutable_fields &&
        !m_config.pure_methods) {
      break;
    }

    auto& cfg = cfg_it.cfg();
    auto primary_insn = cfg.primary_instruction_of_move_result(cfg_it)->insn;
    if (!opcode::is_an_invoke(primary_insn->opcode())) {
      break;
    }
    auto invoked = resolve_method(primary_insn->get_method(),
                                  opcode_to_search(primary_insn));
    if (!invoked) {
      break;
    }
    if (m_config.getter_methods_for_immutable_fields &&
        opcode::is_invoke_virtual(primary_insn->opcode()) &&
        m_config.getter_methods_for_immutable_fields->count(invoked)) {
      replace_with_const(env, cfg_it, xstores, declaring_type);
      break;
    }

    if (m_config.pure_methods &&
        assumenosideeffects(primary_insn->get_method(), invoked)) {
      replace_with_const(env, cfg_it, xstores, declaring_type);
      break;
    }
    break;
  }
  case OPCODE_ADD_INT_LIT:
  case OPCODE_RSUB_INT_LIT:
  case OPCODE_MUL_INT_LIT:
  case OPCODE_AND_INT_LIT:
  case OPCODE_OR_INT_LIT:
  case OPCODE_XOR_INT_LIT:
  case OPCODE_SHL_INT_LIT:
  case OPCODE_SHR_INT_LIT:
  case OPCODE_USHR_INT_LIT:
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
    if (replace_with_const(env, cfg_it, xstores, declaring_type)) {
      break;
    }
    try_simplify(env, cfg_it, m_config, *m_mutation);
    break;
  }

  default: {
  }
  }
}

void Transform::remove_dead_switch(
    const intraprocedural::FixpointIterator& intra_cp,
    const ConstantEnvironment& env,
    cfg::ControlFlowGraph& cfg,
    cfg::Block* block) {

  if (!m_config.remove_dead_switch) {
    return;
  }

  auto insn_it = block->get_last_insn();
  always_assert(insn_it != block->end());
  auto* insn = insn_it->insn;
  always_assert(opcode::is_switch(insn->opcode()));

  // Prune infeasible or unnecessary branches
  cfg::Edge* goto_edge = cfg.get_succ_edge_of_type(block, cfg::EDGE_GOTO);
  cfg::Block* goto_target = goto_edge->target();
  std::unordered_map<cfg::Block*, uint32_t> remaining_branch_targets;
  std::vector<cfg::Edge*> remaining_branch_edges;
  for (auto branch_edge : cfg.get_succ_edges_of_type(block, cfg::EDGE_BRANCH)) {
    auto branch_is_feasible =
        !intra_cp.analyze_edge(branch_edge, env).is_bottom();
    if (branch_is_feasible) {
      remaining_branch_edges.push_back(branch_edge);
      remaining_branch_targets[branch_edge->target()]++;
      continue;
    }
    m_edge_deletes.push_back(branch_edge);
  }

  bool goto_is_feasible = !intra_cp.analyze_edge(goto_edge, env).is_bottom();
  if (!goto_is_feasible && !remaining_branch_targets.empty()) {
    // Rewire infeasible goto to absorb all cases to most common target
    boost::optional<int32_t> most_common_case_key;
    cfg::Block* most_common_target{nullptr};
    uint32_t most_common_target_count{0};
    std::unordered_set<cfg::Block*> visited;
    for (cfg::Edge* e : remaining_branch_edges) {
      auto case_key = *e->case_key();
      auto target = e->target();
      auto count = remaining_branch_targets.at(target);
      always_assert(count > 0);
      if (count > most_common_target_count ||
          (count == most_common_target_count &&
           case_key > *most_common_case_key)) {
        most_common_case_key = case_key;
        most_common_target = target;
        most_common_target_count = count;
      }
    }
    always_assert(most_common_target != nullptr);
    if (most_common_target != goto_target) {
      m_edge_deletes.push_back(goto_edge);
      goto_target = most_common_target;
      m_edge_adds.emplace_back(block, goto_target, cfg::EDGE_GOTO);
      goto_edge = nullptr;
    }
    auto removed = std20::erase_if(remaining_branch_edges, [&](auto* e) {
      if (e->target() == most_common_target) {
        m_edge_deletes.push_back(e);
        return true;
      }
      return false;
    });
    always_assert(removed == most_common_target_count);
    remaining_branch_targets.erase(most_common_target);
    ++m_stats.branches_removed;
    // goto is now feasible
  }

  // When all remaining branches are infeasible, the cfg will remove the switch
  // instruction.
  if (remaining_branch_targets.empty()) {
    ++m_stats.branches_removed;
    return;
  }
  always_assert(!remaining_branch_edges.empty());

  remaining_branch_targets[goto_target]++;
  if (remaining_branch_targets.size() > 1) {
    return;
  }

  always_assert(remaining_branch_targets.size() == 1);
  ++m_stats.branches_removed;
  // Replace the switch by a goto to the uniquely reachable block
  // We do that by deleting all but one of the remaining branch edges, and then
  // the cfg will rewrite the remaining branch into a goto and remove the switch
  // instruction.
  m_edge_deletes.insert(m_edge_deletes.end(),
                        remaining_branch_edges.begin(),
                        remaining_branch_edges.end());
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
  if (opcode::is_switch(insn->opcode())) {
    remove_dead_switch(intra_cp, env, cfg, block);
    return;
  }

  if (!opcode::is_a_conditional_branch(insn->opcode())) {
    return;
  }

  // Get all normal succs (goto/branch edges, excluding ghost edges).
  const auto succs = cfg.get_succ_edges_if(block, [](const auto* e) {
    return e->type() == cfg::EDGE_GOTO || e->type() == cfg::EDGE_BRANCH;
  });
  always_assert_log(succs.size() == 2, "actually %zu\n%s in B%zu:\n%s",
                    succs.size(), SHOW(InstructionIterable(*block)),
                    block->id(), SHOW(cfg));
  for (auto& edge : succs) {
    // Check if the fixpoint analysis has determined the successors to be
    // unreachable
    if (intra_cp.analyze_edge(edge, env).is_bottom()) {
      TRACE(CONSTP, 2, "Removing conditional branch %s", SHOW(insn));
      ++m_stats.branches_removed;
      // We delete the infeasible edge, and then the cfg will rewrite the
      // remaining branch into a goto and remove the if- instruction.
      m_edge_deletes.push_back(edge);
      // Assuming :block is reachable, then at least one of its successors must
      // be reachable, so we can break after finding one that's unreachable
      break;
    }
  }
}

bool Transform::replace_with_throw(
    const ConstantEnvironment& env,
    const cfg::InstructionIterator& cfg_it,
    npe::NullPointerExceptionCreator* npe_creator) {
  auto* insn = cfg_it->insn;
  auto dereferenced_object_src_index = get_dereferenced_object_src_index(insn);
  if (!dereferenced_object_src_index) {
    return false;
  }

  auto reg = insn->src(*dereferenced_object_src_index);
  auto value = env.get(reg).maybe_get<SignedConstantDomain>();
  std::vector<IRInstruction*> new_insns;
  if (!value || !value->get_constant() || *value->get_constant() != 0) {
    return false;
  }

  // We'll replace this instruction with a different instruction sequence that
  // unconditionally throws a null pointer exception.

  m_mutation->replace(cfg_it, npe_creator->get_insns(insn));
  ++m_stats.throws;

  if (insn->has_move_result_any()) {
    auto& cfg = cfg_it.cfg();
    auto move_result_it = cfg.move_result_of(cfg_it);
    if (!move_result_it.is_end()) {
      m_redundant_move_results.insert(move_result_it->insn);
    }
  }
  return true;
}

void Transform::apply_changes(cfg::ControlFlowGraph& cfg) {
  if (!m_edge_adds.empty()) {
    for (auto& t : m_edge_adds) {
      cfg.add_edge(std::get<0>(t), std::get<1>(t), std::get<2>(t));
    }
    m_edge_adds.clear();
  }
  if (!m_edge_deletes.empty()) {
    cfg.delete_edges(m_edge_deletes.begin(), m_edge_deletes.end());
    m_edge_deletes.clear();
  }

  always_assert(m_mutation != nullptr);
  m_mutation->flush();

  if (!m_added_param_values.empty()) {
    // Insert after last load-param (and not before first non-load-param
    // instructions, as that may suggest that the added instructions are to be
    // associated with the position of the non-load-param instruction).
    auto block = cfg.entry_block();
    auto last_load_params_it = block->get_last_param_loading_insn();
    if (last_load_params_it == block->end()) {
      block->push_front(m_added_param_values);
    } else {
      cfg.insert_after(block->to_cfg_instruction_iterator(last_load_params_it),
                       m_added_param_values);
    }
    m_added_param_values.clear();
  }
}

void Transform::apply(const intraprocedural::FixpointIterator& fp_iter,
                      const WholeProgramState& wps,
                      cfg::ControlFlowGraph& cfg,
                      const XStoreRefs* xstores,
                      bool is_static,
                      DexType* declaring_type,
                      DexProto* proto) {
  legacy_apply_constants_and_prune_unreachable(fp_iter, wps, cfg, xstores,
                                               declaring_type);
  if (xstores && !g_redex->instrument_mode) {
    m_stats.unreachable_instructions_removed += cfg.simplify();
    fp_iter.clear_switch_succ_cache();
    // legacy_apply_constants_and_prune_unreachable creates some new blocks that
    // fp_iter isn't aware of. As turns out, legacy_apply_forward_targets
    // doesn't care, and will still do the right thing.
    legacy_apply_forward_targets(fp_iter, cfg, is_static, declaring_type, proto,
                                 xstores);
    m_stats.unreachable_instructions_removed +=
        cfg.remove_unreachable_blocks().first;
  }
}

void Transform::legacy_apply_constants_and_prune_unreachable(
    const intraprocedural::FixpointIterator& intra_cp,
    const WholeProgramState& wps,
    cfg::ControlFlowGraph& cfg,
    const XStoreRefs* xstores,
    const DexType* declaring_type) {
  always_assert(cfg.editable());
  always_assert(m_mutation == nullptr);
  m_mutation = std::make_unique<cfg::CFGMutation>(cfg);
  npe::NullPointerExceptionCreator npe_creator(&cfg);
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    // This block is unreachable, no point mutating its instructions -- DCE
    // will be removing it anyway
    if (env.is_bottom()) {
      continue;
    }
    auto last_insn = block->get_last_insn();
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(); it != ii.end(); it++) {
      auto cfg_it = block->to_cfg_instruction_iterator(it);
      bool any_changes = eliminate_redundant_put(env, wps, cfg_it) ||
                         eliminate_redundant_null_check(env, wps, cfg_it) ||
                         replace_with_throw(env, cfg_it, &npe_creator);
      auto* insn = cfg_it->insn;
      intra_cp.analyze_instruction(insn, &env, insn == last_insn->insn);
      if (!any_changes && !m_redundant_move_results.count(insn)) {
        simplify_instruction(env, wps, cfg_it, xstores, declaring_type);
      }
    }
    eliminate_dead_branch(intra_cp, env, cfg, block);
  }
  apply_changes(cfg);
  m_mutation = nullptr;
  cfg.simplify();
}

void Transform::forward_targets(
    const intraprocedural::FixpointIterator& intra_cp,
    const ConstantEnvironment& env,
    cfg::ControlFlowGraph& cfg,
    cfg::Block* block,
    std::unique_ptr<LivenessFixpointIterator>& liveness_fixpoint_iter) {
  always_assert(!env.is_bottom());
  // normal edges are of type goto or branch, not throw or ghost
  auto is_normal = [](const cfg::Edge* e) {
    return e->type() == cfg::EDGE_GOTO || e->type() == cfg::EDGE_BRANCH;
  };

  // Data structure that holds a possible target block, together with a set out
  // registers that would have been assigned along the way to the target block.
  struct TargetAndAssignedRegs {
    cfg::Block* target;
    std::unordered_set<reg_t> assigned_regs;
  };

  // Helper function that computs (ordered) list of unconditional target blocks,
  // together with the sets of assigned registers.
  auto get_unconditional_targets =
      [&intra_cp, &cfg, &is_normal,
       &env](cfg::Edge* succ_edge) -> std::vector<TargetAndAssignedRegs> {
    auto succ_env = intra_cp.analyze_edge(succ_edge, env);
    if (succ_env.is_bottom()) {
      return {};
    }

    std::vector<TargetAndAssignedRegs> unconditional_targets{
        (TargetAndAssignedRegs){succ_edge->target(), {}}};
    std::unordered_set<cfg::Block*> visited;
    while (true) {
      auto& last_unconditional_target = unconditional_targets.back();
      auto succ = last_unconditional_target.target;
      if (!visited.insert(succ).second) {
        // We found a loop; give up.
        return {};
      }
      // We'll have to add to the set of assigned regs, so we make an
      // intentional copy here
      auto assigned_regs = last_unconditional_target.assigned_regs;
      auto last_insn = succ->get_last_insn();
      for (auto& mie : InstructionIterable(succ)) {
        auto insn = mie.insn;
        if (opcode::is_branch(insn->opcode())) {
          continue;
        }
        // TODO: Support side-effect-free instruction sequences involving
        // move-result(-pseudo), similar to what LocalDCE does
        if (opcode::has_side_effects(insn->opcode()) || !insn->has_dest() ||
            opcode::is_move_result_any(insn->opcode())) {
          TRACE(CONSTP, 5, "forward_targets cannot follow %s",
                SHOW(insn->opcode()));
          // We stop the analysis here.
          return unconditional_targets;
        }

        assigned_regs.insert(insn->dest());
        intra_cp.analyze_instruction(insn, &succ_env, insn == last_insn->insn);
        always_assert(!succ_env.is_bottom());
      }

      boost::optional<std::pair<cfg::Block*, ConstantEnvironment>>
          only_feasible;
      for (auto succ_succ_edge : cfg.get_succ_edges_if(succ, is_normal)) {
        auto succ_succ_env = intra_cp.analyze_edge(succ_succ_edge, succ_env);
        if (succ_succ_env.is_bottom()) {
          continue;
        }
        if (only_feasible) {
          // Found another one that's feasible, so there's not just a single
          // feasible successor. We stop the analysis here.
          return unconditional_targets;
        }
        only_feasible = std::make_pair(succ_succ_edge->target(), succ_succ_env);
      }
      unconditional_targets.push_back(
          (TargetAndAssignedRegs){only_feasible->first, assigned_regs});
      succ_env = only_feasible->second;
    }
  };

  // Helper to check if any assigned register is live at the target block
  auto is_any_assigned_reg_live_at_target =
      [&liveness_fixpoint_iter,
       &cfg](const TargetAndAssignedRegs& unconditional_target) {
        auto& assigned_regs = unconditional_target.assigned_regs;
        if (assigned_regs.empty()) {
          return false;
        }
        if (!liveness_fixpoint_iter) {
          liveness_fixpoint_iter.reset(new LivenessFixpointIterator(cfg));
          liveness_fixpoint_iter->run(LivenessDomain());
        }
        const auto& live_in_vars = liveness_fixpoint_iter->get_live_in_vars_at(
            unconditional_target.target);
        if (live_in_vars.is_bottom()) {
          // Could happen after having applied other transformations already
          return true;
        }
        always_assert(!live_in_vars.is_top());
        auto& elements = live_in_vars.elements();
        return std::find_if(assigned_regs.begin(), assigned_regs.end(),
                            [&elements](reg_t reg) {
                              return elements.contains(reg);
                            }) != assigned_regs.end();
      };

  // Helper function to find furthest feasible target block for which no
  // assigned regs are live-in
  auto get_furthest_target_without_live_assigned_regs =
      [&is_any_assigned_reg_live_at_target](
          const std::vector<TargetAndAssignedRegs>& unconditional_targets)
      -> cfg::Block* {
    // The first (if any) unconditional target isn't interesting, as that's the
    // one that's already currently on the cfg edge
    if (unconditional_targets.size() <= 1) {
      return nullptr;
    }

    // Find last successor where no assigned reg is live
    for (int i = unconditional_targets.size() - 1; i >= 1; --i) {
      auto& unconditional_target = unconditional_targets.at(i);
      if (is_any_assigned_reg_live_at_target(unconditional_target)) {
        continue;
      }
      TRACE(CONSTP, 2,
            "forward_targets rewrites target, skipping %d targets, discharged "
            "%zu assigned regs",
            i, unconditional_target.assigned_regs.size());
      return unconditional_target.target;
    }
    return nullptr;
  };

  // Main loop over, analyzing and potentially rewriting all normal successor
  // edges to the furthest unconditional feasible target
  for (auto succ_edge : cfg.get_succ_edges_if(block, is_normal)) {
    auto unconditional_targets = get_unconditional_targets(succ_edge);
    auto new_target =
        get_furthest_target_without_live_assigned_regs(unconditional_targets);
    if (!new_target) {
      continue;
    }
    // Found (last) successor where no assigned reg is live -- forward to
    // there
    cfg.set_edge_target(succ_edge, new_target);
    ++m_stats.branches_forwarded;
  }
  // TODO: Forwarding may leave behind trivial conditional branches that can
  // be folded.
}

bool Transform::has_problematic_return(cfg::ControlFlowGraph& cfg,
                                       bool is_static,
                                       DexType* declaring_type,
                                       DexProto* proto,
                                       const XStoreRefs* xstores) {
  // Nothing to check without method information
  if (!declaring_type || !proto) {
    return false;
  }

  // No return issues when rtype is primitive
  auto rtype = proto->get_rtype();
  if (type::is_primitive(rtype)) {
    return false;
  }

  // No return issues when there are no try/catch blocks
  auto blocks = cfg.blocks();
  bool has_catch =
      std::find_if(blocks.begin(), blocks.end(), [](cfg::Block* block) {
        return block->is_catch();
      }) != blocks.end();
  if (!has_catch) {
    return false;
  }

  // For all return instructions, check whether the reaching definitions are of
  // a type that's unavailable/external, or defined in a different store.
  auto declaring_class_idx = xstores->get_store_idx(declaring_type);
  auto is_problematic_return_type = [&](const DexType* t, IRInstruction* insn) {
    t = type::get_element_type_if_array(t);
    if (!type_class_internal(t)) {
      // An unavailable or external class
      TRACE(CONSTP, 2,
            "Skipping {%s::%s} because {%s} is unavailable/external in {%s}",
            SHOW(declaring_type), SHOW(proto), SHOW(t), SHOW(insn));
      return true;
    }
    if (!xstores) {
      return false;
    }
    auto t_idx = xstores->get_store_idx(t);
    if (t_idx == declaring_class_idx) {
      return false;
    }
    TRACE(CONSTP, 2,
          "Skipping {%s::%s} because {%s} is from different store (%zu vs %zu) "
          "in "
          "{%s}",
          SHOW(declaring_type), SHOW(proto), SHOW(t), declaring_class_idx,
          t_idx, SHOW(insn));
    return true;
  };
  reaching_defs::MoveAwareFixpointIterator fp_iter(cfg);
  fp_iter.run({});
  std::unique_ptr<type_inference::TypeInference> ti;
  for (cfg::Block* block : blocks) {
    auto env = fp_iter.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    for (auto& mie : InstructionIterable(block)) {
      IRInstruction* insn = mie.insn;
      if (opcode::is_a_return(insn->opcode())) {
        auto defs = env.get(insn->src(0));
        always_assert(!defs.is_bottom() && !defs.is_top());
        for (auto def : defs.elements()) {
          auto op = def->opcode();
          if (def->has_type()) {
            if (is_problematic_return_type(def->get_type(), def)) {
              return true;
            }
          } else if (def->has_method()) {
            always_assert(opcode::is_an_invoke(op));
            if (is_problematic_return_type(
                    def->get_method()->get_proto()->get_rtype(), def)) {
              return true;
            }
          } else if (op == OPCODE_IGET_OBJECT || op == OPCODE_SGET_OBJECT) {
            if (is_problematic_return_type(def->get_field()->get_type(), def)) {
              return true;
            }
          } else if (op == OPCODE_AGET_OBJECT) {
            if (!ti) {
              ti.reset(new type_inference::TypeInference(cfg));
              ti->run(is_static, declaring_type, proto->get_args());
            }
            auto& type_environments = ti->get_type_environments();
            auto& type_environment = type_environments.at(def);
            auto dex_type = type_environment.get_dex_type(def->src(1));
            if (dex_type && type::is_array(*dex_type) &&
                is_problematic_return_type(
                    type::get_array_component_type(*dex_type), def)) {
              return true;
            }
          }
        }
      }
      fp_iter.analyze_instruction(insn, &env);
    }
  }
  return false;
}

void Transform::legacy_apply_forward_targets(
    const intraprocedural::FixpointIterator& intra_cp,
    cfg::ControlFlowGraph& cfg,
    bool is_static,
    DexType* declaring_type,
    DexProto* proto,
    const XStoreRefs* xstores) {
  cfg.calculate_exit_block();

  // The following is an attempt to avoid creating a control-flow structure that
  // triggers the Android bug described in T55782799, related to a return
  // statement in a try region when a type is unavailable/external, possibly
  // from a different store.
  // Besides that Android bug, it really shouldn't be necessary to do anything
  // special about unavailable types or cross-store references here.
  if (has_problematic_return(cfg, is_static, declaring_type, proto, xstores)) {
    return;
  }

  // Note that the given intra_cp might not be aware of all blocks that exist in
  // the cfg.
  std::unique_ptr<LivenessFixpointIterator> liveness_fixpoint_iter;
  for (auto block : cfg.blocks()) {
    const auto& env = intra_cp.get_exit_state_at(block);
    if (env.is_bottom()) {
      // We found an unreachable block, or one that was added the cfg after
      // intra_cp has run; just ignore it.
      continue;
    }
    forward_targets(intra_cp, env, cfg, block, liveness_fixpoint_iter);
  }
}

void Transform::Stats::log_metrics(ScopedMetrics& sm, bool with_scope) const {
  using OptScope = boost::optional<ScopedMetrics::Scope>;
  OptScope scope = with_scope ? OptScope(sm.scope("const_prop")) : boost::none;
  sm.set_metric("branches_forwarded", branches_forwarded);
  sm.set_metric("branch_propagated", branches_removed);
  sm.set_metric("materialized_consts", materialized_consts);
  sm.set_metric("throws", throws);
  sm.set_metric("null_checks", null_checks);
  sm.set_metric("null_checks_method_calls", null_checks_method_calls);
  sm.set_metric("unreachable_instructions_removed",
                unreachable_instructions_removed);
  sm.set_metric("redundant_puts_removed", redundant_puts_removed);
  TRACE(CONSTP, 3, "Null checks removed: %zu(%zu)", null_checks,
        null_checks_method_calls);
  sm.set_metric("added_param_const", added_param_const);
}

} // namespace constant_propagation
