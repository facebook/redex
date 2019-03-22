/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <queue>
#include <vector>
#include <unordered_map>

#include "ControlFlow.h"
#include "IRCode.h"
#include "Show.h"
#include "Tool.h"
#include "Walkers.h"

namespace {

using LogicalBlock = std::vector<std::vector<cfg::Block*>>;

/**
 * Return true if the block ends with a throw instruction.
 */
bool is_throw_block(const DexMethod* meth, cfg::Block* block) {
  // check last instruction of a block to see if it's a throw
  for (auto insn = block->rbegin(); insn != block->rend(); ++insn) {
    if (insn->type != MFLOW_OPCODE) continue;
    if (insn->insn->opcode() == OPCODE_THROW) {
      // debug only, a block ending with a throw should
      // only have catch successors
      if (block->succs().size() > 0) {
        for (const auto& succ : block->succs()) {
          if (!succ->target()->is_catch()) {
            always_assert_log(false,
                "throw block with successors in %s",
                SHOW(meth));
          }
        }
      }
      return true;
    }
    break;
  }
  return false;
}

/**
 * Count proper instruction in a block, for some concept of proper.
 * If switches and array initialization are around this is not
 * doing the right thing.
 */
int block_size(const std::vector<cfg::Block*>& logical_block) {
  int count = 0;
  for (const auto& block : logical_block) {
    for (auto insn = block->begin(); insn != block->end(); ++insn) {
      if (insn->type == MFLOW_OPCODE) {
        count++;
      }
    }
  }
  return count;
}

/**
 * Print how many blocks have a given number of instructions.
 */
void print_blocks_by_size(const LogicalBlock& throwing_blocks) {
  const int MAX_COUNT = 50;
  int block_insn_count[MAX_COUNT];
  for (int i = 0; i < MAX_COUNT; i++) {
    block_insn_count[i] = 0;
  }
  for (const auto& block : throwing_blocks) {
    int count = block_size(block);
    if (count > MAX_COUNT) {
      block_insn_count[MAX_COUNT - 1]++;
    } else {
      block_insn_count[count - 1]++;
    }
  }
  for (int i = 0; i < MAX_COUNT; i++) {
    if (block_insn_count[i] > 0) {
      fprintf(stderr, "%d blocks with %d instructions\n",
          block_insn_count[i], i + 1);
    }
  }
}

/**
 * Collect all predecessors which are not part of any return block.
 * This should basically merge all blocks that are part of the same
 * throw path.
 */
void walk_predecessors(
    cfg::Block* block,
    std::vector<cfg::Block*>& throw_code,
    std::unordered_set<cfg::Block*>& left_blocks) {
  throw_code.emplace_back(block);
  const auto& preds = block->preds();
  for (const auto& pred_edge : preds) {
    auto* pred = pred_edge->src();
    if (left_blocks.count(pred) > 0) {
      left_blocks.erase(pred);
      walk_predecessors(pred, throw_code, left_blocks);
    }
  }
}

/**
 * Collect all the blocks leading to a throw and contributing to the
 * throw only.
 */
void collect_throwing_blocks(
    DexMethod* meth, LogicalBlock& throwing_blocks) {
  const auto& blocks = meth->get_code()->cfg().blocks();
  std::queue<cfg::Block*> blocks_to_visit;
  std::unordered_set<cfg::Block*> no_throw_blocks;
  // collect all blocks with a return
  for (const auto& block : blocks) {
    for (auto insn = block->rbegin(); insn != block->rend(); ++insn) {
      if (insn->type == MFLOW_OPCODE) {
        if (is_return(insn->insn->opcode())) {
          blocks_to_visit.push(block);
          no_throw_blocks.insert(block);
        }
        break;
      }
    }
  }
  // get all preds of returning blocks until no more blocks are found
  while (!blocks_to_visit.empty()) {
    const auto block = blocks_to_visit.front();
    blocks_to_visit.pop();
    for (const auto& pred_edge : block->preds()) {
      auto pred = pred_edge->src();
      if (no_throw_blocks.count(pred) == 0) {
        blocks_to_visit.push(pred);
        no_throw_blocks.insert(pred);
      }
    }
  }
  if (blocks.size() == no_throw_blocks.size()) {
    // I beleive this happens if a method throws and catches within
    // the method, needs some investigation
    fprintf(stderr, "throw blocks reachable from return in %s\n", SHOW(meth));
    return;
  }
  // collect all remaining blocks
  std::unordered_set<cfg::Block*> left_blocks;
  std::queue<cfg::Block*> throw_blocks;
  for (const auto& block : blocks) {
    if (no_throw_blocks.count(block) > 0) continue;
    if (is_throw_block(meth, block)) {
      throw_blocks.push(block);
    } else {
      left_blocks.insert(block);
    }
  }
  // in the remaining blocks find the one that throw and walk
  // predeccesors to see which belong to the same throwing block
  while (!throw_blocks.empty()) {
    const auto block = throw_blocks.front();
    throw_blocks.pop();
    std::vector<cfg::Block*> throw_code;
    walk_predecessors(block, throw_code, left_blocks);
    throwing_blocks.push_back(std::move(throw_code));
  }
}

/**
 * Find all blocks that are in a throwing path.
 * Effectively looks for blocks that terminate with a throw and
 * if one is found the function is analyzed to determine all blocks
 * that are part of the throing path only.
 */
void find_throwing_block(const Scope& scope) {
  LogicalBlock throwing_blocks;
  walk::code(scope,
      [&](DexMethod* meth, IRCode& code) {
        code.build_cfg(/* editable */ false);
        const auto& cfg = code.cfg();
        for (const auto& block : cfg.blocks()) {
          if (is_throw_block(meth, block)) {
            // do the analysis to find the blocks contributing to the throw
            collect_throwing_blocks(meth, throwing_blocks);
          }
        }
      });
  fprintf(stderr, "throwing blocks %ld\n", throwing_blocks.size());
  print_blocks_by_size(throwing_blocks);
}

}

class AnalyzeThrows : public Tool {
 public:
  AnalyzeThrows() : Tool("analyze-throws", "analyze blocks ending with throws") {}

  void add_options(po::options_description& options) const override {
    add_standard_options(options);
  }

  void run(const po::variables_map& options) override {
    auto stores = init(
      options["jars"].as<std::string>(),
      options["apkdir"].as<std::string>(),
      options["dexendir"].as<std::string>());
    const auto& scope = build_class_scope(stores);
    find_throwing_block(scope);
  }

 private:
};

static AnalyzeThrows s_tool;
