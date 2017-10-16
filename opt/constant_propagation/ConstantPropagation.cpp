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

void IntraProcConstantPropagation::apply_changes(DexMethod* method) const {
  auto code = method->get_code();
  for (auto const& p : m_lcp.insn_replacements()) {
    IRInstruction* const& old_op = p.first;
    IRInstruction* const& new_op = p.second;
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
    rcp.apply_changes(method);

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
