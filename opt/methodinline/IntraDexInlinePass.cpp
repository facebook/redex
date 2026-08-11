/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IntraDexInlinePass.h"
#include "ConfigFiles.h"

#include "Debug.h"
#include "MethodInliner.h"
#include "PassManager.h"

void IntraDexInlinePass::bind_config() {
  std::string hot_cold_inlining_behavior_str;
  bind("hot_cold_inlining_behavior", "none", hot_cold_inlining_behavior_str);
  bind("partial_hot_hot", false, m_partial_hot_hot);
  bind("profile_guided", false, m_profile_guided);
  bind("profile_guided_heat_threshold", 0.5f, m_profile_guided_heat_threshold);
  bind("profile_guided_heat_discount", 1.0f, m_profile_guided_heat_discount);
  bind("profile_guided_shrink_bias", 0.0f, m_profile_guided_shrink_bias);
  bind("profile_guided_block_appear_threshold", 0.0f,
       m_profile_guided_block_appear_threshold);
  bind("inline_hot_callsite_count_percentile",
       DEFAULT_COST_CONFIG.inline_hot_callsite_count_percentile,
       m_inline_hot_callsite_count_percentile,
       "If in (0,100], a callsite whose block execution count is at or above "
       "this rank percentile (high = hot; 95 = hottest 5%) of profiled "
       "(positive-count) callsites gets its local inline cost scaled by "
       "inline_hot_callsite_count_discount. Any value <= 0 (the default, -1) "
       "disables (NFC).");
  bind("inline_hot_callsite_count_discount",
       DEFAULT_COST_CONFIG.inline_hot_callsite_count_discount,
       m_inline_hot_callsite_count_discount,
       "Multiplier (< 1 favors inlining) applied to the local inline cost of a "
       "callsite selected as hot by inline_hot_callsite_count_percentile; only "
       "used when that lever is > 0. With a ramp configured this is the "
       "discount at inline_hot_callsite_count_percentile, not a flat value.");
  bind("inline_hot_callsite_count_top_percentile",
       DEFAULT_COST_CONFIG.inline_hot_callsite_count_top_percentile,
       m_inline_hot_callsite_count_top_percentile,
       "Percentile at which inline_hot_callsite_count_top_discount is reached. "
       "Between it and inline_hot_callsite_count_percentile the discount ramps "
       "geometrically. Unset (-1) gives a flat step instead of a ramp.");
  bind("inline_hot_callsite_count_top_discount",
       DEFAULT_COST_CONFIG.inline_hot_callsite_count_top_discount,
       m_inline_hot_callsite_count_top_discount,
       "Discount at inline_hot_callsite_count_top_percentile, i.e. the cap. "
       "Must be <= inline_hot_callsite_count_discount.");
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
  if (m_profile_guided) {
    inliner_cost_config.profile_guided_heat_threshold =
        m_profile_guided_heat_threshold;
    inliner_cost_config.profile_guided_heat_discount =
        m_profile_guided_heat_discount;
    inliner_cost_config.profile_guided_shrink_bias =
        m_profile_guided_shrink_bias;
    inliner_cost_config.profile_guided_block_appear_threshold =
        m_profile_guided_block_appear_threshold;
  }

  // Hot-callsite count-percentile lever (same as MethodInlinePass, here scoped
  // to the intra-dex inliner run). Defaults reproduce DEFAULT_COST_CONFIG
  // (percentile -1 = off), so this is NFC unless a config sets it.
  inliner_cost_config.inline_hot_callsite_count_percentile =
      m_inline_hot_callsite_count_percentile;
  inliner_cost_config.inline_hot_callsite_count_discount =
      m_inline_hot_callsite_count_discount;
  inliner_cost_config.inline_hot_callsite_count_top_percentile =
      m_inline_hot_callsite_count_top_percentile;
  inliner_cost_config.inline_hot_callsite_count_top_discount =
      m_inline_hot_callsite_count_top_discount;

  inliner::run_inliner(stores, mgr, conf, inliner_cost_config,
                       m_hot_cold_inlining_behavior, m_partial_hot_hot,
                       /* intra_dex */ true, m_profile_guided);
  // For partial inlining, we only consider the first time the pass runs, to
  // avoid repeated partial inlining. (This shouldn't be necessary as the
  // partial inlining fallback invocation is marked as cold, but just in case
  // some other Redex optimization disturbs that hotness data.)
  if (m_partial_hot_hot) {
    m_partial_hot_hot = false;
  }
}

static IntraDexInlinePass s_pass;
