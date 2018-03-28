/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
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
#include "Walkers.h"

namespace {

constexpr const char* METRIC_GOTO_REMOVED = "num_goto_removed";

class RemoveGotos {
 private:
  static bool is_goto(const MethodItemEntry& mie) {
    return mie.type == MFLOW_OPCODE && mie.insn->opcode() == OPCODE_GOTO;
  }

  static bool has_goto(cfg::Block* block_ptr) {
    return is_goto(*std::prev(block_ptr->end()));
  }

  static IRList::iterator find_goto(cfg::Block* block_ptr) {
    auto iter = std::find_if(block_ptr->begin(),
                             block_ptr->end(),
                             [](MethodItemEntry& mei) { return is_goto(mei); });
    always_assert(block_ptr->end() != iter);
    return iter;
  }

  /*
   * A block B is mergeable if and only if
   * - B jumps to another block C unconditionally
   * - C has exactly one parent (B)
   * - C does not fallthrough to the next block implicitly. (e.g copying C into
   * B does not cause any inconsistencies in the CFG)
   */
  static cfg::Block* find_mergeable_block(IRCode* code) {
    code->build_cfg();
    for (cfg::Block* current_block : code->cfg().blocks()) {
      if (current_block->succs().size() != 1 || !has_goto(current_block)) {
        continue;
      }

      cfg::Block* next_block = current_block->succs()[0]->target();
      if (next_block != current_block && next_block->preds().size() == 1 &&
          (next_block->succs().empty() || has_goto(next_block))) {
        return current_block;
      }
    }

    return nullptr;
  }

 public:
  size_t process_method(DexMethod* method) {
    size_t num_goto_removed = 0;
    auto code = method->get_code();
    always_assert(code != nullptr);

    TRACE(RMGOTO, 4, "Class: %s\n", SHOW(method->get_class()));
    TRACE(RMGOTO, 4, "Method: %s\n", SHOW(method->get_name()));
    TRACE(RMGOTO, 4, "Initial opcode count: %d\n", code->count_opcodes());

    cfg::Block* current_block = find_mergeable_block(code);
    while (current_block != nullptr) {
      TRACE(RMGOTO,
            5,
            "Found optimizing pair, parent block id: %d\n",
            current_block->id());
      num_goto_removed++;

      cfg::Block* next_block = current_block->succs()[0]->target();

      std::vector<MethodItemEntry*> next_block_mies;
      auto next_iter = next_block->begin();
      next_iter = code->erase(next_iter);
      while (next_iter != next_block->end()) {
        next_block_mies.push_back(&(*next_iter));
        next_iter = code->erase(next_iter);
      }

      auto goto_iter = find_goto(current_block);
      auto current_iter = goto_iter;
      for (auto& mie : next_block_mies) {
        current_iter = code->insert_after(current_iter, *mie);
      }
      code->erase(goto_iter);

      TRACE(RMGOTO, 5, "Opcode count: %d\n", code->count_opcodes());
      current_block = find_mergeable_block(code);
    }

    TRACE(RMGOTO, 4, "Final opcode count: %d\n", code->count_opcodes());
    return num_goto_removed;
  }
};
} // namespace

size_t RemoveGotosPass::run(DexMethod* method) {
  RemoveGotos rmgotos;
  return rmgotos.process_method(method);
}

void RemoveGotosPass::run_pass(DexStoresVector& stores,
                               ConfigFiles& /* unused */,
                               PassManager& mgr) {
  auto scope = build_class_scope(stores);

  size_t total_gotos_removed = walk::parallel::reduce_methods<RemoveGotos, size_t>(
      scope,
      [](RemoveGotos& rmgotos, DexMethod* m) -> size_t {
        if (!m->get_code()) {
          return 0;
        }
        return rmgotos.process_method(m);
      },
      [](size_t a, size_t b) { return a + b; },
      [](unsigned int /*thread_index*/) { return RemoveGotos(); });
  mgr.incr_metric(METRIC_GOTO_REMOVED, total_gotos_removed);
  TRACE(RMGOTO,
        1,
        "Number of unnecessary gotos removed: %d\n",
        total_gotos_removed);
}

static RemoveGotosPass s_pass;
