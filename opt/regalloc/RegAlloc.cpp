/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RegAlloc.h"

#include "Debug.h"
#include "DexUtil.h"
#include "GraphColoring.h"
#include "PassManager.h"
#include "RegisterAllocation.h"
#include "Trace.h"
#include "Walkers.h"

namespace regalloc {

using Stats = graph_coloring::Allocator::Stats;

void RegAllocPass::eval_pass(DexStoresVector&, ConfigFiles&, PassManager&) {
  ++m_eval;
}

void RegAllocPass::run_pass(DexStoresVector& stores,
                            ConfigFiles&,
                            PassManager& mgr) {
  graph_coloring::Allocator::Config allocator_config;
  const auto& jw = mgr.get_current_pass_info()->config;
  jw.get("live_range_splitting", false, allocator_config.use_splitting);
  allocator_config.no_overwrite_this =
      mgr.get_redex_options().no_overwrite_this();

  auto scope = build_class_scope(stores);
  auto stats = walk::parallel::methods<Stats>(scope, [&](DexMethod* m) {
    return graph_coloring::allocate(allocator_config, m);
  });

  TRACE(REG, 1, "Total reiteration count: %lu", stats.reiteration_count);
  TRACE(REG, 1, "Total Params spilled early: %lu", stats.params_spill_early);
  TRACE(REG, 1, "Total spill count: %lu", stats.moves_inserted());
  TRACE(REG, 1, "  Total param spills: %lu", stats.param_spill_moves);
  TRACE(REG, 1, "  Total range spills: %lu", stats.range_spill_moves);
  TRACE(REG, 1, "  Total global spills: %lu", stats.global_spill_moves);
  TRACE(REG, 1, "  Total splits: %lu", stats.split_moves);
  TRACE(REG, 1, "Total coalesce count: %lu", stats.moves_coalesced);
  TRACE(REG, 1, "Total net moves: %ld", stats.net_moves());

  mgr.incr_metric("param spilled too early", stats.params_spill_early);
  mgr.incr_metric("reiteration_count", stats.reiteration_count);
  mgr.incr_metric("spill_count", stats.moves_inserted());
  mgr.incr_metric("coalesce_count", stats.moves_coalesced);
  mgr.incr_metric("net_moves", stats.net_moves());

  ++m_run;
  // For the last invocation, record that final register allocation has been
  // done.
  if (m_eval == m_run) {
    TRACE(REG, 1, "Marking final register allocation");
    mgr.record_running_regalloc();
  }
}

static RegAllocPass s_pass;

} // namespace regalloc
