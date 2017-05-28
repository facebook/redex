/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "RegAlloc.h"

#include <boost/functional/hash.hpp>

#include "Dataflow.h"
#include "DexUtil.h"
#include "GraphColoring.h"
#include "IRInstruction.h"
#include "LiveRange.h"
#include "Transform.h"
#include "Walkers.h"

using namespace regalloc;

/*
 * Pick the opcode that can address the largest register operands. This gives
 * the register allocator more flexibility in allocating the corresponding live
 * ranges. The IR -> DexCode conversion that runs later will pick the smallest
 * possible opcodes for the given operands, essentially undoing this operation
 * if it is found to be unnecessary.
 */
static DexOpcode pessimize_opcode(DexOpcode op) {
  switch (op) {
  case OPCODE_MOVE:
  case OPCODE_MOVE_FROM16:
    return OPCODE_MOVE_16;
  case OPCODE_MOVE_OBJECT:
  case OPCODE_MOVE_OBJECT_FROM16:
    return OPCODE_MOVE_OBJECT_16;
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_WIDE_FROM16:
    return OPCODE_MOVE_WIDE_16;
  case OPCODE_CONST_4:
  case OPCODE_CONST_16:
  case OPCODE_CONST_HIGH16:
    return OPCODE_CONST;
  case OPCODE_CONST_WIDE_HIGH16:
  case OPCODE_CONST_WIDE_16:
  case OPCODE_CONST_WIDE_32:
    return OPCODE_CONST_WIDE;
  default:
    return op;
  }
}

void RegAllocPass::run_pass(DexStoresVector& stores,
                            ConfigFiles&,
                            PassManager& mgr) {
  auto scope = build_class_scope(stores);
  graph_coloring::Allocator::Stats stats;
  walk_code(scope,
            [](DexMethod*) { return true; },
            [&](DexMethod* m, IRCode& code) {
              TRACE(REG, 3, "Handling %s:\n", SHOW(m));
              TRACE(REG, 5, "regs:%d code:\n%s\n",
                    code.get_registers_size(),
                    SHOW(&code));
              try {
                for (auto& mie : InstructionIterable(&code)) {
                  mie.insn->range_to_srcs();
                  mie.insn->normalize_registers();
                  mie.insn->set_opcode(pessimize_opcode(mie.insn->opcode()));
                }

                live_range::renumber_registers(&code);
                graph_coloring::Allocator allocator;
                allocator.allocate(&code);
                stats.accumulate(allocator.get_stats());

                TRACE(REG, 5, "After alloc: regs:%d code:\n%s\n",
                      code.get_registers_size(),
                      SHOW(&code));

                for (auto& mie : InstructionIterable(&code)) {
                  mie.insn->denormalize_registers();
                  mie.insn->srcs_to_range();
                }
              } catch (std::exception&) {
                fprintf(stderr, "Failed to allocate %s\n", SHOW(m));
                fprintf(stderr, "%s\n", SHOW(code.cfg()));
                throw;
              }
            });

  TRACE(REG, 1, "Total reiteration count: %lu\n", stats.reiteration_count);
  TRACE(REG, 1, "Total spill count: %lu\n", stats.moves_inserted());
  TRACE(REG, 1, "  Total param spills: %lu\n", stats.param_spill_moves);
  TRACE(REG, 1, "  Total range spills: %lu\n", stats.range_spill_moves);
  TRACE(REG, 1, "  Total global spills: %lu\n", stats.global_spill_moves);
  TRACE(REG, 1, "Total coalesce count: %lu\n", stats.moves_coalesced);
  TRACE(REG, 1, "Total net moves: %ld\n", stats.net_moves());

  mgr.incr_metric("reiteration_count", stats.reiteration_count);
  mgr.incr_metric("spill_count", stats.moves_inserted());
  mgr.incr_metric("coalesce_count", stats.moves_coalesced);
  mgr.incr_metric("net_moves", stats.net_moves());
}

static RegAllocPass s_pass;
