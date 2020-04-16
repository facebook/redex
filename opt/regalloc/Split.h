/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "IRCode.h"
#include "Interference.h"
#include "Liveness.h"

namespace regalloc {

using vreg_t = uint16_t;

enum BlockMode { FALLTHROUGH, BRANCH, TRYCATCH };

struct SplitConstraints {
  // Map of catch blocks and number of incoming control flow edges on
  // which a given register dies.
  std::unordered_map<cfg::Block*, size_t> catch_blocks;
  // Map of non-catch blocks and number of incoming control flow edges on
  // which a given register dies.
  std::unordered_map<cfg::Block*, size_t> other_blocks;
  // Set of MethodItemEntry of invoke-xxx or fill-new-array before move-result
  // if the move-result's dest is the given register.
  std::unordered_set<MethodItemEntry*> write_result;
  // Number of store needed if we split this given register.
  size_t split_store{0};
  // Number of Load needed if we split this given register.
  size_t split_load{0};
};

struct SplitCosts {
  std::unordered_map<vreg_t, SplitConstraints> reg_constraints;

  size_t total_value_at(vreg_t u) const {
    const SplitConstraints& load_store = reg_constraints.at(u);
    return load_store.split_store + load_store.split_load;
  }

  const std::unordered_map<cfg::Block*, size_t>& death_at_catch(
      vreg_t u) const {
    return reg_constraints.at(u).catch_blocks;
  }

  const std::unordered_map<cfg::Block*, size_t>& death_at_other(
      vreg_t u) const {
    return reg_constraints.at(u).other_blocks;
  }

  const std::unordered_set<MethodItemEntry*>& get_write_result(vreg_t u) const {
    return reg_constraints.at(u).write_result;
  }

  void increase_load(vreg_t u) { ++reg_constraints[u].split_load; }

  void increase_store(vreg_t u) { ++reg_constraints[u].split_store; }

  void add_catch_block(vreg_t u, cfg::Block* catch_block) {
    ++reg_constraints[u].catch_blocks[catch_block];
  }

  void add_other_block(vreg_t u, cfg::Block* other_block) {
    ++reg_constraints[u].other_blocks[other_block];
  }

  void add_write_result(vreg_t u, MethodItemEntry* invoke_filled) {
    reg_constraints[u].write_result.emplace(invoke_filled);
  }
};

struct SplitPlan {
  // A map between reg and a set of registers that will split around reg.
  std::unordered_map<vreg_t, std::unordered_set<vreg_t>> split_around;
};

struct BlockModeInsn {
  std::unordered_set<IRInstruction*> block_insns;
  BlockMode block_mode;

  void add_insn_mode(IRInstruction* insn, BlockMode mode) {
    block_mode = mode;
    block_insns.emplace(insn);
  }
};

struct BlockLoadInfo {
  using BlockEdge = std::pair<cfg::Block*, cfg::Block*>;

  struct block_edge_comparator {
    bool operator()(const BlockEdge& b1, const BlockEdge& b2) const {
      if (b1.first->id() != b2.first->id()) {
        return b1.first->id() < b2.first->id();
      }
      return b1.second->id() < b2.second->id();
    }
  };

  // Map of catch blocks and registers already loaded in these blocks.
  std::unordered_map<cfg::Block*, std::unordered_set<vreg_t>> try_loaded_regs;
  // Map of non-catch blocks and registers already loaded in these blocks.
  std::unordered_map<cfg::Block*, std::unordered_set<vreg_t>> other_loaded_regs;
  // Map of the edges between two blocks and what their type is and load
  // instructions we should inserted for these edges.
  // This is an ordered map because we iterate through it.
  std::map<BlockEdge, BlockModeInsn, block_edge_comparator> mode_and_insn;
  // Map of branch edges between two blocks and pairs of MethodItemEntry of
  // BRANCH instruction and branch target.
  std::unordered_map<BlockEdge,
                     std::pair<MethodItemEntry*, MethodItemEntry*>,
                     boost::hash<BlockEdge>>
      target_branch;
};

using namespace interference;

// Count load and store for possible split
void calc_split_costs(const LivenessFixpointIterator&, IRCode*, SplitCosts*);

size_t split(const LivenessFixpointIterator&,
             const SplitPlan&,
             const SplitCosts&,
             const Graph&,
             IRCode*);

} // namespace regalloc
