/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IntraDexInlinePass.h"

#include "MethodInliner.h"

void IntraDexInlinePass::bind_config() {
  std::string hot_cold_inlining_behavior_str;
  bind("hot_cold_inlining_behavior", "none", hot_cold_inlining_behavior_str);
  bind("partial_hot_hot", false, m_partial_hot_hot);
  bind("baseline_profile_guided", false, m_baseline_profile_guided);
  bind("baseline_profile_heat_threshold", 0.5f,
       m_baseline_profile_heat_threshold);
  bind("baseline_profile_heat_discount", 1.0f,
       m_baseline_profile_heat_discount);
  bind("baseline_profile_shrink_bias", 0.0f, m_baseline_profile_shrink_bias);
  after_configuration([this, hot_cold_inlining_behavior_str =
                                 std::move(hot_cold_inlining_behavior_str)]() {
    always_assert(!hot_cold_inlining_behavior_str.empty());
    m_hot_cold_inlining_behavior = inliner::parse_hot_cold_inlining_behavior(
        hot_cold_inlining_behavior_str);
  });
}

void IntraDexInlinePass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  InlinerCostConfig inliner_cost_config = DEFAULT_COST_CONFIG;
  if (m_baseline_profile_guided) {
    inliner_cost_config.profile_guided_heat_threshold =
        m_baseline_profile_heat_threshold;
    inliner_cost_config.profile_guided_heat_discount =
        m_baseline_profile_heat_discount;
    inliner_cost_config.profile_guided_shrink_bias =
        m_baseline_profile_shrink_bias;
  }

  inliner::run_inliner(stores, mgr, conf, inliner_cost_config,
                       m_hot_cold_inlining_behavior, m_partial_hot_hot,
                       /* intra_dex */ true, m_baseline_profile_guided);
  // For partial inlining, we only consider the first time the pass runs, to
  // avoid repeated partial inlining. (This shouldn't be necessary as the
  // partial inlining fallback invocation is marked as cold, but just in case
  // some other Redex optimization disturbs that hotness data.)
  if (m_partial_hot_hot) {
    m_partial_hot_hot = false;
  }
}

static IntraDexInlinePass s_pass;
