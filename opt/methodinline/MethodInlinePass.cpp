/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodInlinePass.h"

void MethodInlinePass::bind_config() {
  size_t cost_invoke;
  bind("cost_invoke",
       (size_t)(DEFAULT_COST_CONFIG.cost_invoke * 100),
       cost_invoke);
  m_inliner_cost_config.cost_invoke = (float)cost_invoke / (float)100;
  size_t cost_move_result;
  bind("cost_move_result",
       (size_t)(DEFAULT_COST_CONFIG.cost_move_result * 100),
       cost_move_result);
  m_inliner_cost_config.cost_move_result = (float)cost_move_result / (float)100;
  bind("cost_method",
       DEFAULT_COST_CONFIG.cost_method,
       m_inliner_cost_config.cost_method);
  bind("unused_args_discount",
       DEFAULT_COST_CONFIG.unused_args_discount,
       m_inliner_cost_config.unused_args_discount);
  bind("reg_threshold_1",
       DEFAULT_COST_CONFIG.reg_threshold_1,
       m_inliner_cost_config.reg_threshold_1);
  bind("reg_threshold_2",
       DEFAULT_COST_CONFIG.reg_threshold_2,
       m_inliner_cost_config.reg_threshold_2);
  bind("op_init_class_cost",
       DEFAULT_COST_CONFIG.op_init_class_cost,
       m_inliner_cost_config.op_init_class_cost);
  bind("op_injection_id_cost",
       DEFAULT_COST_CONFIG.op_injection_id_cost,
       m_inliner_cost_config.op_injection_id_cost);
  bind("op_unreachable_cost",
       DEFAULT_COST_CONFIG.op_unreachable_cost,
       m_inliner_cost_config.op_unreachable_cost);
  bind("op_move_exception_cost",
       DEFAULT_COST_CONFIG.op_move_exception_cost,
       m_inliner_cost_config.op_move_exception_cost);
  bind("insn_cost_1",
       DEFAULT_COST_CONFIG.insn_cost_1,
       m_inliner_cost_config.insn_cost_1);
  bind("insn_has_data_cost",
       DEFAULT_COST_CONFIG.insn_has_data_cost,
       m_inliner_cost_config.insn_has_data_cost);
  bind("insn_has_lit_cost_1",
       DEFAULT_COST_CONFIG.insn_has_lit_cost_1,
       m_inliner_cost_config.insn_has_lit_cost_1);
  bind("insn_has_lit_cost_2",
       DEFAULT_COST_CONFIG.insn_has_lit_cost_2,
       m_inliner_cost_config.insn_has_lit_cost_2);
  bind("insn_has_lit_cost_3",
       DEFAULT_COST_CONFIG.insn_has_lit_cost_3,
       m_inliner_cost_config.insn_has_lit_cost_3);
}

void MethodInlinePass::run_pass(DexStoresVector& stores,
                                ConfigFiles& conf,
                                PassManager& mgr) {
  inliner::run_inliner(stores, mgr, conf, m_inliner_cost_config);
}

static MethodInlinePass s_pass;
