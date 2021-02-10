/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationTransform.h"

#include "ReachingDefinitions.h"
#include "Transform.h"
#include "TypeInference.h"

namespace constant_propagation {

/*
 * Replace an instruction that has a single destination register with a `const`
 * load. `env` holds the state of the registers after `insn` has been
 * evaluated. So, `env.get(dest)` holds the _new_ value of the destination
 * register.
 */
void Transform::replace_with_const(const ConstantEnvironment& env,
                                   const IRList::iterator& it,
                                   const XStoreRefs* xstores,
                                   const DexType* declaring_type) {
  auto* insn = it->insn;
  auto value = env.get(insn->dest());
  auto replacement = ConstantValue::apply_visitor(
      value_to_instruction_visitor(insn, xstores, declaring_type), value);
  if (replacement.empty()) {
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
                                     const IRList::iterator& it,
                                     const XStoreRefs* xstores,
                                     const DexType* declaring_type) {
  auto* insn = it->insn;
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
    const IRList::iterator& it) {
  auto* insn = it->insn;
  switch (insn->opcode()) {
  case OPCODE_INVOKE_STATIC: {
    if (auto index =
            get_null_check_object_index(insn, m_kotlin_null_check_assertions)) {
      auto val = env.get(insn->src(*index)).maybe_get<SignedConstantDomain>();
      if (val && val->interval() == sign_domain::Interval::NEZ) {
        m_deletes.push_back(it);
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
                                     const IRList::iterator& it,
                                     const XStoreRefs* xstores,
                                     const DexType* declaring_type) {
  auto* insn = it->insn;
  switch (insn->opcode()) {
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE: {
    generate_const_param(env, it, xstores, declaring_type);
    break;
  }
  case OPCODE_MOVE:
  case OPCODE_MOVE_WIDE:
    if (m_config.replace_moves_with_consts) {
      replace_with_const(env, it, xstores, declaring_type);
    }
    break;
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT: {
    auto* primary_insn = ir_list::primary_instruction_of_move_result_pseudo(it);
    auto op = primary_insn->opcode();
    if (is_sget(op) || is_iget(op) || is_aget(op) || is_div_int_lit(op) ||
        is_rem_int_lit(op) || is_instance_of(op) || is_rem_int_or_long(op) ||
        is_div_int_or_long(op) || is_check_cast(op)) {
      replace_with_const(env, it, xstores, declaring_type);
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
      replace_with_const(env, it, xstores, declaring_type);
    } else if (m_config.getter_methods_for_immutable_fields) {
      auto primary_insn = ir_list::primary_instruction_of_move_result(it);
      if (is_invoke_virtual(primary_insn->opcode())) {
        auto invoked =
            resolve_method(primary_insn->get_method(), MethodSearch::Virtual);
        if (m_config.getter_methods_for_immutable_fields->count(invoked)) {
          replace_with_const(env, it, xstores, declaring_type);
        }
      }
    }
    break;
  }
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
    replace_with_const(env, it, xstores, declaring_type);
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
  always_assert_log(succs.size() == 2, "actually %zu\n%s", succs.size(),
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

Transform::Stats Transform::apply_on_uneditable_cfg(
    const intraprocedural::FixpointIterator& intra_cp,
    const WholeProgramState& wps,
    IRCode* code,
    const XStoreRefs* xstores,
    const DexType* declaring_type) {
  auto& cfg = code->cfg();
  boost::optional<int32_t> temp_reg;
  for (const auto& block : cfg.blocks()) {
    auto env = intra_cp.get_entry_state_at(block);
    // This block is unreachable, no point mutating its instructions -- DCE
    // will be removing it anyway
    if (env.is_bottom()) {
      continue;
    }
    auto last_insn = block->get_last_insn();
    for (auto& mie : InstructionIterable(block)) {
      auto it = code->iterator_to(mie);
      bool any_changes = eliminate_redundant_put(env, wps, it) ||
                         eliminate_redundant_null_check(env, wps, it) ||
                         replace_with_throw(env, it, code, &temp_reg);
      auto* insn = mie.insn;
      intra_cp.analyze_instruction(insn, &env, insn == last_insn->insn);
      if (!any_changes && !m_redundant_move_results.count(insn)) {
        simplify_instruction(env, wps, code->iterator_to(mie), xstores,
                             declaring_type);
      }
    }
    eliminate_dead_branch(intra_cp, env, cfg, block);
  }
  apply_changes(code);
  return m_stats;
}

void Transform::forward_targets(
    const intraprocedural::FixpointIterator& intra_cp,
    const ConstantEnvironment& env,
    cfg::ControlFlowGraph& cfg,
    cfg::Block* block,
    std::unique_ptr<LivenessFixpointIterator>& liveness_fixpoint_iter) {
  if (env.is_bottom()) {
    // we found an unreachable block; ignore it
    return;
  }

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
        if (is_branch(insn->opcode())) {
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
        auto live_in_vars = liveness_fixpoint_iter->get_live_in_vars_at(
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
            "forward_targets rewrites target, skipping %zu targets, discharged "
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
                                       DexMethod* method,
                                       const XStoreRefs* xstores) {
  // Nothing to check without method information
  if (!method) {
    return false;
  }

  // No return issues when rtype is primitive
  auto rtype = method->get_proto()->get_rtype();
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
  auto declaring_class_idx = xstores->get_store_idx(method->get_class());
  auto is_problematic_return_type = [&](const DexType* t, IRInstruction* insn) {
    t = type::get_element_type_if_array(t);
    if (!type_class_internal(t)) {
      // An unavailable or external class
      TRACE(CONSTP, 2,
            "Skipping {%s} because {%s} is unavailable/external in {%s}",
            SHOW(method), SHOW(t), SHOW(insn));
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
          "Skipping {%s} because {%s} is from different store (%zu vs %zu) in "
          "{%s}",
          SHOW(method), SHOW(t), declaring_class_idx, t_idx, SHOW(insn));
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
      if (is_return(insn->opcode())) {
        auto defs = env.get(insn->src(0));
        always_assert(!defs.is_bottom() && !defs.is_top());
        for (auto def : defs.elements()) {
          auto op = def->opcode();
          if (def->has_type()) {
            if (is_problematic_return_type(def->get_type(), def)) {
              return true;
            }
          } else if (def->has_method()) {
            always_assert(is_invoke(op));
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
              ti->run(method);
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

Transform::Stats Transform::apply(
    const intraprocedural::FixpointIterator& intra_cp,
    cfg::ControlFlowGraph& cfg,
    DexMethod* method,
    const XStoreRefs* xstores) {
  // The following is an attempt to avoid creating a control-flow structure that
  // triggers the Android bug described in T55782799, related to a return
  // statement in a try region when a type is unavailable/external, possibly
  // from a different store.
  // Besides that Android bug, it really shouldn't be necessary to do anything
  // special about unavailable types or cross-store references here.
  if (has_problematic_return(cfg, method, xstores)) {
    return m_stats;
  }

  std::unique_ptr<LivenessFixpointIterator> liveness_fixpoint_iter;
  for (auto block : cfg.blocks()) {
    auto env = intra_cp.get_exit_state_at(block);
    forward_targets(intra_cp, env, cfg, block, liveness_fixpoint_iter);
  }
  return m_stats;
}

} // namespace constant_propagation
