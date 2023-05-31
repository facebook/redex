/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass sorts non-perf sensitive classes according to their inheritance
 * hierarchies in each dex. This improves compressibility.
 */
#include "MethodSplittingPass.h"

#include "ConfigFiles.h"
#include "InterDexPass.h"
#include "MethodSplitter.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"

using namespace method_splitting_impl;

void MethodSplittingPass::bind_config() {
  bind("split_block_size", m_config.split_block_size, m_config.split_block_size,
       "Splits blocks so that no block has more opcodes than this size");
  bind("min_original_size", m_config.min_original_size,
       m_config.min_original_size,
       "Minimum size of method to consider splitting");
  bind("min_original_size_too_large_for_inlining",
       m_config.min_original_size_too_large_for_inlining,
       m_config.min_original_size_too_large_for_inlining,
       "Minimum size of method to consider splitting when too large for "
       "inlining");
  bind("min_hot_split_size", m_config.min_hot_split_size,
       m_config.min_hot_split_size, "Minimum size of split-out hot code");
  bind("min_hot_cold_split_size", m_config.min_hot_cold_split_size,
       m_config.min_hot_cold_split_size,
       "Minimum size of split-out code with transition from hot to cold");
  bind("min_cold_split_size", m_config.min_cold_split_size,
       m_config.min_cold_split_size, "Minimum size of split-out code code");
  bind("huge_threshold", m_config.huge_threshold, m_config.huge_threshold,
       "Threshold for a method to be considered huge to activate "
       "max_huge_overhead_ratio");
  bind("max_overhead_ratio", m_config.max_overhead_ratio,
       m_config.max_overhead_ratio,
       "Maximum ratio of combined split and remaining code size vs "
       "original code size");
  bind("max_huge_overhead_ratio", m_config.max_huge_overhead_ratio,
       m_config.max_huge_overhead_ratio,
       "Maximum ratio of combined split and remaining code size vs "
       "original code size for huge methods");
  bind("max_live_in", m_config.max_live_in, m_config.max_live_in,
       "Maximum number of live-in registers");
  bind("max_iteration", m_config.max_iteration, m_config.max_iteration,
       "Maximum number of top-level iterations");
}

void MethodSplittingPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& conf,
                                   PassManager& mgr) {
  const auto& interdex_metrics = mgr.get_interdex_metrics();
  auto it = interdex_metrics.find(interdex::METRIC_RESERVED_MREFS);
  size_t reserved_mrefs = it == interdex_metrics.end() ? 0 : it->second;
  it = interdex_metrics.find(interdex::METRIC_RESERVED_TREFS);
  size_t reserved_trefs = it == interdex_metrics.end() ? 0 : it->second;

  auto name_infix = "$" + std::to_string(m_iteration) + "$";
  Stats stats;
  split_methods_in_stores(stores, mgr.get_redex_options().min_sdk, m_config,
                          conf.create_init_class_insns(), reserved_mrefs,
                          reserved_trefs, &stats, name_infix);

  mgr.set_metric("split_count", stats.added_methods.size());
  mgr.set_metric("split_count_simple", (size_t)stats.split_count_simple);
  mgr.set_metric("split_count_switches", (size_t)stats.split_count_switches);
  mgr.set_metric("split_count_switch_cases",
                 (size_t)stats.split_count_switch_cases);
  mgr.set_metric("hot_split_count", (size_t)stats.hot_split_count);
  mgr.set_metric("hot_cold_split_count", (size_t)stats.hot_cold_split_count);
  mgr.set_metric("cold_split_count", (size_t)stats.cold_split_count);
  mgr.set_metric("dex_limits_hit", (size_t)stats.dex_limits_hit);
  mgr.set_metric("added_code_size", (size_t)stats.added_code_size);
  mgr.set_metric("split_code_size", (size_t)stats.split_code_size);
  TRACE(MS, 1, "Split out %zu methods", stats.added_methods.size());

  m_iteration++;
}

static MethodSplittingPass s_pass;
