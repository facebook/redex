/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RegAlloc.h"

#include <boost/functional/hash.hpp>

#include "Dataflow.h"
#include "DexUtil.h"
#include "GraphColoring.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "LiveRange.h"
#include "Transform.h"
#include "Walkers.h"

#include "JemallocUtil.h"

using namespace regalloc;

void RegAllocPass::run_pass(DexStoresVector& stores,
                            ConfigFiles&,
                            PassManager& mgr) {
  using Output = graph_coloring::Allocator::Stats;
  auto scope = build_class_scope(stores);
  auto stats = walk::parallel::reduce_methods<Output>(
      scope,
      [this](DexMethod* m) { // mapper
        graph_coloring::Allocator::Stats stats;
        if (m->get_code() == nullptr) {
          return stats;
        }
        auto& code = *m->get_code();

        TRACE(REG, 3, "Handling %s:\n", SHOW(m));
        TRACE(REG,
              5,
              "regs:%d code:\n%s\n",
              code.get_registers_size(),
              SHOW(&code));
        try {
          // The transformations below all require a CFG. Build it once
          // here instead of requiring each transform to build it.
          code.build_cfg();
          // It doesn't make sense to try to allocate registers in
          // unreachable code. Remove it so that the allocator doesn't
          // get confused.
          code.cfg().remove_unreachable_blocks();
          code.clear_cfg();
          code.build_cfg(false);
          live_range::renumber_registers(&code, /* width_aware */ false);
          graph_coloring::Allocator allocator(m_allocator_config);
          allocator.allocate(m);
          stats.accumulate(allocator.get_stats());

          TRACE(REG,
                5,
                "After alloc: regs:%d code:\n%s\n",
                code.get_registers_size(),
                SHOW(&code));
        } catch (std::exception&) {
          fprintf(stderr, "Failed to allocate %s\n", SHOW(m));
          fprintf(stderr, "%s\n", SHOW(code.cfg()));
          throw;
        }
        return stats;
      },
      [](Output a, Output b) { // reducer
        a.accumulate(b);
        return a;
      });

  TRACE(REG, 1, "Total reiteration count: %lu\n", stats.reiteration_count);
  TRACE(REG, 1, "Total Params spilled early: %lu\n", stats.params_spill_early);
  TRACE(REG, 1, "Total spill count: %lu\n", stats.moves_inserted());
  TRACE(REG, 1, "  Total param spills: %lu\n", stats.param_spill_moves);
  TRACE(REG, 1, "  Total range spills: %lu\n", stats.range_spill_moves);
  TRACE(REG, 1, "  Total global spills: %lu\n", stats.global_spill_moves);
  TRACE(REG, 1, "  Total splits: %lu\n", stats.split_moves);
  TRACE(REG, 1, "Total coalesce count: %lu\n", stats.moves_coalesced);
  TRACE(REG, 1, "Total net moves: %ld\n", stats.net_moves());

  mgr.incr_metric("param spilled too early", stats.params_spill_early);
  mgr.incr_metric("reiteration_count", stats.reiteration_count);
  mgr.incr_metric("spill_count", stats.moves_inserted());
  mgr.incr_metric("coalesce_count", stats.moves_coalesced);
  mgr.incr_metric("net_moves", stats.net_moves());

  mgr.record_running_regalloc();
}

static RegAllocPass s_pass;
