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
#include "ParallelWalkers.h"

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
  m_lcp.analyze_instruction(insn, current_state);
}

/*
 * If we can determine that a branch is not taken based on the constants in the
 * environment, set the environment to bottom upon entry into the unreachable
 * block.
 */
static void analyze_if(const IRInstruction* inst,
                       ConstPropEnvironment* current_state,
                       bool is_true_branch) {
  // Inverting the conditional here means that we only need to consider the
  // "true" case of the if-* opcode
  auto op = !is_true_branch ? opcode::invert_conditional_branch(inst->opcode())
                            : inst->opcode();
  // We handle if-eq(z) cases specially since we can infer useful information
  // even if only one operand is constant
  if (op == OPCODE_IF_EQ) {
    auto left_value = current_state->get(inst->src(0));
    auto right_value = current_state->get(inst->src(1));
    auto refined_value = left_value.meet(right_value);
    current_state->set(inst->src(0), refined_value);
    current_state->set(inst->src(1), refined_value);
    return;
  } else if (op == OPCODE_IF_EQZ) {
    auto value = current_state->get(inst->src(0));
    auto refined_value = value.meet(
        ConstantDomain::value(0, ConstantValue::ConstantType::NARROW));
    current_state->set(inst->src(0), refined_value);
    return;
  }

  int32_t left_value;
  int32_t right_value;
  if (!get_constant_value_at_src(*current_state,
                                 inst,
                                 /* src_idx */ 0,
                                 /* default_value */ 0,
                                 left_value)) {
    return;
  }
  if (!get_constant_value_at_src(*current_state,
                                 inst,
                                 /* src_idx */ 1,
                                 /* default_value */ 0,
                                 right_value)) {
    return;
  }

  switch (op) {
    case OPCODE_IF_NE:
    case OPCODE_IF_NEZ:
      if (left_value == right_value) {
        current_state->set_to_bottom();
      }
      break;
    case OPCODE_IF_LT:
    case OPCODE_IF_LTZ:
      if (left_value >= right_value) {
        current_state->set_to_bottom();
      }
      break;
    case OPCODE_IF_GE:
    case OPCODE_IF_GEZ:
      if (left_value < right_value) {
        current_state->set_to_bottom();
      }
      break;
    case OPCODE_IF_GT:
    case OPCODE_IF_GTZ:
      if (left_value <= right_value) {
        current_state->set_to_bottom();
      }
      break;
    case OPCODE_IF_LE:
    case OPCODE_IF_LEZ:
      if (left_value > right_value) {
        current_state->set_to_bottom();
      }
      break;
    default:
      always_assert_log(false, "expected if-* opcode");
      not_reached();
  }
}

static IRInstruction* find_last_instruction(Block* block) {
  for (auto it = block->rbegin(); it != block->rend(); ++it) {
    if (it->type == MFLOW_OPCODE) {
      return it->insn;
    }
  }
  return nullptr;
}

ConstPropEnvironment IntraProcConstantPropagation::analyze_edge(
    Block* const& source,
    Block* const& destination,
    const ConstPropEnvironment& exit_state_at_source) const {
  auto current_state = exit_state_at_source;
  if (!m_config.propagate_conditions) {
    return current_state;
  }

  IRInstruction* last_insn = find_last_instruction(source);
  if (last_insn == nullptr) {
    return current_state;
  }

  auto op = last_insn->opcode();
  if (is_conditional_branch(op)) {
    analyze_if(
        last_insn,
        &current_state,
        /* if_true_branch */ m_cfg.edge(source, destination)[EDGE_BRANCH]);
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

  walk_methods_parallel_simple(scope, [&](DexMethod* method) {
    if (!method->get_code()) {
      return;
    }
    // Skipping blacklisted classes
    if (m_config.blacklist.count(method->get_class()) > 0) {
      TRACE(CONSTP, 2, "Skipping %s\n", SHOW(method));
      return;
    }

    TRACE(CONSTP, 5, "Class: %s\n", SHOW(method->get_class()));
    TRACE(CONSTP, 5, "Method: %s\n", SHOW(method->get_name()));

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
