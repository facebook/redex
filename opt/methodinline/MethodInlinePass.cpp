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
  bind("cross_dex_penalty_coe1",
       DEFAULT_COST_CONFIG.cross_dex_penalty_coe1,
       m_inliner_cost_config.cross_dex_penalty_coe1);
  bind("cross_dex_penalty_coe2",
       DEFAULT_COST_CONFIG.cross_dex_penalty_coe2,
       m_inliner_cost_config.cross_dex_penalty_coe2);
  bind("cross_dex_penalty_const",
       DEFAULT_COST_CONFIG.cross_dex_penalty_const,
       m_inliner_cost_config.cross_dex_penalty_const);
  bind("cross_dex_bonus_const",
       DEFAULT_COST_CONFIG.cross_dex_bonus_const,
       m_inliner_cost_config.cross_dex_bonus_const);
  bind("unused_arg_zero_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_zero_multiplier,
       m_inliner_cost_config.unused_arg_zero_multiplier);
  bind("unused_arg_non_zero_constant_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_non_zero_constant_multiplier,
       m_inliner_cost_config.unused_arg_non_zero_constant_multiplier);
  bind("unused_arg_nez_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_nez_multiplier,
       m_inliner_cost_config.unused_arg_nez_multiplier);
  bind("unused_arg_interval_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_interval_multiplier,
       m_inliner_cost_config.unused_arg_interval_multiplier);
  bind("unused_arg_singleton_object_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_singleton_object_multiplier,
       m_inliner_cost_config.unused_arg_singleton_object_multiplier);
  bind("unused_arg_object_with_immutable_attr_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_object_with_immutable_attr_multiplier,
       m_inliner_cost_config.unused_arg_object_with_immutable_attr_multiplier);
  bind("unused_arg_string_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_string_multiplier,
       m_inliner_cost_config.unused_arg_string_multiplier);
  bind("unused_arg_class_object_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_class_object_multiplier,
       m_inliner_cost_config.unused_arg_class_object_multiplier);
  bind("unused_arg_new_object_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_new_object_multiplier,
       m_inliner_cost_config.unused_arg_new_object_multiplier);
  bind("unused_arg_other_object_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_other_object_multiplier,
       m_inliner_cost_config.unused_arg_other_object_multiplier);
  bind("unused_arg_not_top_multiplier",
       DEFAULT_COST_CONFIG.unused_arg_not_top_multiplier,
       m_inliner_cost_config.unused_arg_not_top_multiplier);
  bind("consider_hot_cold", false, m_consider_hot_cold);
  bind("partial_hot_hot", false, m_partial_hot_hot);
}

void MethodInlinePass::run_pass(DexStoresVector& stores,
                                ConfigFiles& conf,
                                PassManager& mgr) {
  inliner::run_inliner(stores, mgr, conf, m_inliner_cost_config,
                       m_consider_hot_cold, m_partial_hot_hot);
  // For partial inlining, we only consider the first time the pass runs, to
  // avoid repeated partial inlining. (This shouldn't be necessary as the
  // partial inlining fallback invocation is marked as cold, but just in case
  // some other Redex optimization disturbs that hotness data.)
  if (m_partial_hot_hot) {
    m_partial_hot_hot = false;
  }
}

static MethodInlinePass s_pass;
