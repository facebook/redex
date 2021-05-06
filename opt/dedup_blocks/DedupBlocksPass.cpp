/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DedupBlocksPass.h"

#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
const char* METRIC_BLOCKS_REMOVED = "blocks_removed";
const char* METRIC_BLOCKS_SPLIT = "blocks_split";
const char* METRIC_ELIGIBLE_BLOCKS = "eligible_blocks";
} // namespace

void DedupBlocksPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* unused */,
                               PassManager& mgr) {
  const auto& scope = build_class_scope(stores);

  const auto stats = walk::parallel::methods<dedup_blocks_impl::Stats>(
      scope,
      [&](DexMethod* method) {
        const auto code = method->get_code();
        if (code == nullptr || m_config.method_blocklist.count(method) != 0) {
          return dedup_blocks_impl::Stats();
        }

        TRACE(DEDUP_BLOCKS, 3, "[dedup blocks] method %s", SHOW(method));

        code->build_cfg(/* editable */ true);
        auto& cfg = code->cfg();

        TRACE(DEDUP_BLOCKS, 5, "[dedup blocks] method %s before:\n%s",
              SHOW(method), SHOW(cfg));

        dedup_blocks_impl::DedupBlocks impl(&m_config, method);
        impl.run();

        code->clear_cfg();
        return impl.get_stats();
      },
      m_config.debug ? 1 : redex_parallel::default_num_threads());

  report_stats(mgr, stats);
}

void DedupBlocksPass::report_stats(PassManager& mgr,
                                   const dedup_blocks_impl::Stats& stats) {
  int eligible_blocks = stats.eligible_blocks;
  int removed = stats.blocks_removed;
  int split = stats.blocks_split;
  mgr.incr_metric(METRIC_ELIGIBLE_BLOCKS, eligible_blocks);
  mgr.incr_metric(METRIC_BLOCKS_REMOVED, removed);
  mgr.incr_metric(METRIC_BLOCKS_SPLIT, split);
  TRACE(DEDUP_BLOCKS, 2, "%d eligible_blocks", eligible_blocks);

  for (const auto& entry : stats.dup_sizes) {
    TRACE(DEDUP_BLOCKS,
          2,
          "found %zu duplicate blocks with %zu instructions",
          entry.second,
          entry.first);
  }

  TRACE(DEDUP_BLOCKS, 1, "%d blocks split", split);
  TRACE(DEDUP_BLOCKS, 1, "%d blocks removed", removed);
}

static DedupBlocksPass s_pass;
