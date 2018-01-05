/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "ConstantPropagation.h"

#include "DexUtil.h"
#include "GlobalConstProp.h"
#include "LocalConstProp.h"
#include "Transform.h"
#include "Walkers.h"

using namespace constant_propagation_impl;
using std::placeholders::_1;
using std::string;
using std::vector;

void IntraProcConstantPropagation::simplify_instruction(
    Block* const& block,
    MethodItemEntry& mie,
    const ConstPropEnvironment& current_state) const {
  auto insn = mie.insn;
  m_lcp.simplify_instruction(insn, current_state);
}

void IntraProcConstantPropagation::analyze_instruction(
    const MethodItemEntry& mie, ConstPropEnvironment* current_state) const {
  auto insn = mie.insn;
  auto op = insn->opcode();
  if (opcode::is_load_param(op)) {
    // We assume that the initial environment passed to run() has parameter
    // bindings already added, so do nothing here
    return;
  } else {
    m_lcp.analyze_instruction(insn, current_state);
  }
}

/*
 * If we can determine that a branch is not taken based on the constants in the
 * environment, set the environment to bottom upon entry into the unreachable
 * block.
 */
static void analyze_if(const IRInstruction* insn,
                       ConstPropEnvironment* state,
                       bool is_true_branch) {
  if (state->is_bottom()) {
    return;
  }
  // Inverting the conditional here means that we only need to consider the
  // "true" case of the if-* opcode
  auto op = !is_true_branch ? opcode::invert_conditional_branch(insn->opcode())
                            : insn->opcode();

  auto scd_left = state->get(insn->src(0));
  auto scd_right = insn->srcs_size() > 1
                       ? state->get(insn->src(1))
                       : SignedConstantDomain(0, ConstantValue::NARROW);

  switch (op) {
  case OPCODE_IF_EQ: {
    auto refined_value = scd_left.meet(scd_right);
    state->set(insn->src(0), refined_value);
    state->set(insn->src(1), refined_value);
    break;
  }
  case OPCODE_IF_EQZ: {
    state->set(insn->src(0),
               scd_left.meet(SignedConstantDomain(0, ConstantValue::NARROW)));
    break;
  }
  case OPCODE_IF_NE:
  case OPCODE_IF_NEZ: {
    auto cd_left = scd_left.constant_domain();
    auto cd_right = scd_right.constant_domain();
    if (!(cd_left.is_value() && cd_right.is_value())) {
      break;
    }
    if (cd_left.value().constant() == cd_right.value().constant()) {
      state->set_to_bottom();
    }
    break;
  }
  case OPCODE_IF_LT:
    if (scd_left.min_element() >= scd_right.max_element()) {
      state->set_to_bottom();
    }
    break;
  case OPCODE_IF_LTZ:
    state->set(insn->src(0),
               scd_left.meet(SignedConstantDomain(sign_domain::Interval::LTZ)));
    break;
  case OPCODE_IF_GE:
    if (scd_left.max_element() < scd_right.min_element()) {
      state->set_to_bottom();
    }
    break;
  case OPCODE_IF_GEZ:
    state->set(insn->src(0),
               scd_left.meet(SignedConstantDomain(sign_domain::Interval::GEZ)));
    break;
  case OPCODE_IF_GT:
    if (scd_left.max_element() <= scd_right.min_element()) {
      state->set_to_bottom();
    }
    break;
  case OPCODE_IF_GTZ:
    state->set(insn->src(0),
               scd_left.meet(SignedConstantDomain(sign_domain::Interval::GTZ)));
    break;
  case OPCODE_IF_LE:
    if (scd_left.min_element() > scd_right.max_element()) {
      state->set_to_bottom();
    }
    break;
  case OPCODE_IF_LEZ:
    state->set(insn->src(0),
               scd_left.meet(SignedConstantDomain(sign_domain::Interval::LEZ)));
    break;
  default:
    always_assert_log(false, "expected if-* opcode, got %s", SHOW(insn));
    not_reached();
  }
}

ConstPropEnvironment IntraProcConstantPropagation::analyze_edge(
    const std::shared_ptr<cfg::Edge>& edge,
    const ConstPropEnvironment& exit_state_at_source) const {
  auto current_state = exit_state_at_source;
  if (!m_config.propagate_conditions) {
    return current_state;
  }

  auto last_insn_it = transform::find_last_instruction(edge->src());
  if (last_insn_it == edge->src()->end()) {
    return current_state;
  }

  auto insn = last_insn_it->insn;
  auto op = insn->opcode();
  if (is_conditional_branch(op)) {
    analyze_if(insn, &current_state, edge->type() == EDGE_BRANCH);
  }
  return current_state;
}

void IntraProcConstantPropagation::apply_changes(IRCode* code) const {
  for (auto const& p : m_lcp.insn_replacements()) {
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

void ConstantPropagationPass::configure_pass(const PassConfig& pc) {
  pc.get(
      "replace_moves_with_consts", false, m_config.replace_moves_with_consts);
  pc.get("fold_arithmetic", false, m_config.fold_arithmetic);
  pc.get("propagate_conditions", false, m_config.propagate_conditions);
  vector<string> blacklist_names;
  pc.get("blacklist", {}, blacklist_names);

  for (auto const& name : blacklist_names) {
    DexType* entry = DexType::get_type(name.c_str());
    if (entry) {
      TRACE(CONSTP, 2, "Blacklisted class: %s\n", SHOW(entry));
      m_config.blacklist.insert(entry);
    }
  }
}

void ConstantPropagationPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles&,
                                       PassManager& mgr) {
  auto scope = build_class_scope(stores);

  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (!method->get_code()) {
      return;
    }
    // Skipping blacklisted classes
    if (m_config.blacklist.count(method->get_class()) > 0) {
      TRACE(CONSTP, 2, "Skipping %s\n", SHOW(method));
      return;
    }

    TRACE(CONSTP, 2, "Method: %s\n", SHOW(method));

    auto code = method->get_code();
    code->build_cfg();
    auto& cfg = code->cfg();

    TRACE(CONSTP, 5, "CFG: %s\n", SHOW(cfg));
    IntraProcConstantPropagation rcp(cfg, m_config);
    rcp.run(ConstPropEnvironment());
    rcp.simplify();
    rcp.apply_changes(code);

    {
      std::lock_guard<std::mutex> lock{m_stats_mutex};
      m_branches_removed += rcp.branches_removed();
      m_materialized_consts += rcp.materialized_consts();
    }
  });

  mgr.incr_metric("num_branch_propagated", m_branches_removed);
  mgr.incr_metric("num_materialized_consts", m_materialized_consts);

  TRACE(CONSTP, 1, "num_branch_propagated: %d\n", m_branches_removed);
  TRACE(CONSTP, 1, "num_moves_replaced_by_const_loads: %d\n", m_materialized_consts);
}

static ConstantPropagationPass s_pass;
