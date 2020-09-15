/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This optimizer pass removes gotos that are chained together by rearranging
 * the instruction blocks to be in order (as opposed to jumping around).
 */

#include "RemoveGotos.h"

#include <algorithm>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_GOTO_REMOVED = "num_goto_removed";

class RemoveGotos {
 private:
  /*
   * A blocks B and C can be merged together if and only if
   * - B jumps to C unconditionally
   * - C's only predecessor is B
   * - B and C both point to the same catch handler
   *
   * If current_block (B) has a mergable child (C), return C.
   * Otherwise, return nullptr
   */
  static cfg::Block* mergable_child(cfg::ControlFlowGraph& cfg,
                                    cfg::Block* current_block) {
    const auto& succs = current_block->succs();
    if (succs.size() != 1) {
      return nullptr;
    }
    const auto& edge = succs[0];
    if (edge->type() != cfg::EDGE_GOTO) {
      return nullptr;
    }

    cfg::Block* next_block = edge->target();
    if (next_block != current_block && next_block != cfg.entry_block() &&
        next_block->preds().size() == 1 &&
        cfg.blocks_are_in_same_try(current_block, next_block)) {
      return next_block;
    }
    return nullptr;
  }

  static std::vector<cfg::Block*> get_mergable_blocks(
      cfg::ControlFlowGraph& cfg, cfg::Block* first_block) {
    std::vector<cfg::Block*> result;

    result.emplace_back(first_block);
    for (auto block = mergable_child(cfg, first_block); block != nullptr;
         block = mergable_child(cfg, block)) {
      result.emplace_back(block);
    }

    return result;
  }

  /*
   * Returns the number of blocks that were removed
   */
  static size_t merge_blocks(cfg::ControlFlowGraph& cfg) {
    std::unordered_set<cfg::Block*> visited_blocks;

    size_t num_merged = 0;
    // We can safely delete blocks while iterating through because cfg.blocks()
    // copies the block pointers into a vector.
    for (cfg::Block* block : cfg.blocks()) {
      if (visited_blocks.count(block) == 0) {
        const auto& chain = get_mergable_blocks(cfg, block);
        visited_blocks.insert(chain.begin(), chain.end());
        if (chain.size() > 1) {
          TRACE_NO_LINE(RMGOTO, 3, "Found optimizing chain: { ");
          for (const auto& b : chain) {
            TRACE_NO_LINE(RMGOTO, 3, "%d ", b->id());
          }
          TRACE(RMGOTO, 3, "}");

          // Traverse in reverse order because the child block is deleted
          auto prev = chain.rend();
          for (auto it = chain.rbegin(); it != chain.rend(); prev = it++) {
            if (prev != chain.rend()) {
              TRACE(RMGOTO, 3, "merge %d into %d", (*prev)->id(), (*it)->id());
              // merge *prev into *it
              cfg.merge_blocks(*it, *prev);
            }
          }
          num_merged += chain.size() - 1;
        }
      }
    }
    return num_merged;
  }

 public:
  static size_t process_method(DexMethod* method) {
    auto code = method->get_code();

    TRACE(RMGOTO, 4, "Class: %s", SHOW(method->get_class()));
    TRACE(RMGOTO, 3, "Method: %s", SHOW(method->get_name()));
    auto init_opcode_count = code->count_opcodes();
    TRACE(RMGOTO, 4, "Initial opcode count: %d", init_opcode_count);

    TRACE(RMGOTO, 3, "input code\n%s", SHOW(code));
    code->build_cfg(/* editable */ true);
    auto& cfg = code->cfg();

    TRACE(RMGOTO, 3, "before %s", SHOW(cfg));

    size_t num_goto_removed = merge_blocks(cfg);

    TRACE(RMGOTO, 3, "%d blocks merged", num_goto_removed);
    TRACE(RMGOTO, 3, "after %s", SHOW(cfg));
    TRACE(RMGOTO, 5, "Opcode count: %d", code->count_opcodes());

    code->clear_cfg();
    auto final_opcode_count = code->count_opcodes();
    if (final_opcode_count > init_opcode_count) {
      TRACE(RMGOTO,
            3,
            "method %s got larger: (%d -> %d)",
            SHOW(method),
            init_opcode_count,
            final_opcode_count);
    }
    TRACE(RMGOTO, 4, "Final opcode count: %d", code->count_opcodes());
    TRACE(RMGOTO, 3, "output code\n%s", SHOW(code));
    return num_goto_removed;
  }
};
} // namespace

size_t RemoveGotosPass::run(DexMethod* method) {
  return RemoveGotos::process_method(method);
}

void RemoveGotosPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* unused */,
                               PassManager& mgr) {
  auto scope = build_class_scope(stores);

  size_t total_gotos_removed =
      walk::parallel::methods<size_t>(scope, [](DexMethod* m) -> size_t {
        if (!m->get_code()) {
          return 0;
        }
        return RemoveGotos::process_method(m);
      });

  mgr.incr_metric(METRIC_GOTO_REMOVED, total_gotos_removed);
  TRACE(RMGOTO, 1, "Number of unnecessary gotos removed: %d",
        total_gotos_removed);
}

static RemoveGotosPass s_pass;
