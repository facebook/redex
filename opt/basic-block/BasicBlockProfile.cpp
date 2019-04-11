/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BasicBlockProfile.h"

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "DexClass.h"
#include "DexUtil.h"
#include "Match.h"
#include "Show.h"
#include "Walkers.h"

/*
 * This pass performs basic block profiling for dynamic (runtime) analysis.
 * This pass collects statistics about basic blocks for each of these
 * methods or each method in listed class.
 */

static size_t num_opcodes_bb(cfg::Block* block) {
  size_t result = 0;
  for (auto i : InstructionIterable(block)) {
    ++result;
  }
  return result;
}

void BasicBlockProfilePass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& cfg,
                                     PassManager& pm) {

  std::unordered_set<cfg::Block*> bb_profiled;

  auto scope = build_class_scope(stores);
  walk::methods(scope, [&](DexMethod* method) {
    IRCode* code = method->get_code();
    if (code == nullptr) {
      return;
    }
    code->build_cfg(/* editable */ false);
    const auto& blocks = code->cfg().blocks();

    TRACE(BBPROFILE, 5, "M,%s,%zu,%zu,%d\n",
          method->get_deobfuscated_name().c_str(), blocks.size(),
          code->count_opcodes(), method->is_virtual());

    for (cfg::Block* block : blocks) {
      // Only if the current block is a multi-sink block, its predecessors are
      // profiled. This is for tracing the path back.
      if (block->preds().size() > 1) {
        for (const auto& pred : block->preds()) {
          bb_profiled.insert(pred->src());
        }
      }
      TRACE(BBPROFILE, 5,
            "B,%zu,%zu,%zu,"
            "%zu,%d\n",
            block->id(), num_opcodes_bb(block), block->succs().size(),
            block->preds().size(), bb_profiled.count(block) != 0);
    }
  });
}

static BasicBlockProfilePass s_pass;
