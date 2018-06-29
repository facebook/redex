/* Copyright (c) Facebook, Inc. and its affiliates.
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
void BasicBlockProfilePass::run_pass(DexStoresVector& stores,
                                     ConfigFiles& cfg,
                                     PassManager& pm) {

  size_t num_blocks = 0;
  size_t sum_block_size = 0;
  size_t sum_fan = 0;
  size_t num_multi_sink = 0;
  size_t num_methods = 0;
  std::unordered_set<cfg::Block*> m_bb_profiled;

  auto scope = build_class_scope(stores);
  walk::methods(scope, [&](DexMethod* method) {
    IRCode* code = method->get_code();
    if (code == nullptr) {
      return;
    }
    num_methods++;
    // Count the number of basic blocks and number of successors and
    // predecessors of each in current method.
    code->build_cfg();
    const auto& blocks = code->cfg().blocks();
    TRACE(BBPROFILE,
          5,
          "Method:: %s Number of Blocks:: %d\n",
          SHOW(method),
          blocks.size());

    num_blocks += blocks.size();
    for (cfg::Block* block : blocks) {
      size_t num_of_instructions{0};
      auto insn_iter = block->begin();
      auto insn_iter_end = block->end();
      // Iterate over all entries in a basic block to check if an entry is an
      // instruction (MFLOW_OPCODE or MFLOW_DEX_OPCODE) and it is not an
      // internal instruction.
      while (insn_iter++ != insn_iter_end) {
        if ((insn_iter->type == MFLOW_OPCODE ||
             insn_iter->type == MFLOW_DEX_OPCODE)) { //&&
          // !opcode::is_internal(insn_iter->insn->opcode())) {
          num_of_instructions++;
        }
      }
      sum_block_size += num_of_instructions;
      auto num_of_preds = block->preds().size();
      sum_fan += num_of_preds;
      // Only if the current block is a multi-sink block, its predecessors are
      // profiled. This is for tracing the path back.
      if (num_of_preds > 1) {
        num_multi_sink++;
        for (const auto& pred : block->preds()) {
          m_bb_profiled.insert(pred->src());
        }
      }
      // Temporary tracing information used for debugging only.
      TRACE(BBPROFILE, 5,
            "Id: %zu, Num Succs: %zu Num Preds: %zu, Num of instructions in "
            "block: % zu, Num of instructions in method: % zu\n ",
            block->id(), block->succs().size(), num_of_preds,
            num_of_instructions, code->count_opcodes());
    }
  });

  // Final Statistics.
  TRACE(BBPROFILE,
        1,
        "Average: Blocks/Method: %.2f, Block Size- %.2f, Fan- %.2f Percent "
        "MultiSink: %.2f, Percent "
        "MultiSInk Preds: %.2f",
        (float)num_blocks / (float)num_methods,
        (float)sum_block_size / (float)num_blocks,
        (float)sum_fan / (float)num_blocks,
        (100 * (float)num_multi_sink) / (float)num_blocks,
        (100 * (float)m_bb_profiled.size() / (float)num_blocks));
}

static BasicBlockProfilePass s_pass;
