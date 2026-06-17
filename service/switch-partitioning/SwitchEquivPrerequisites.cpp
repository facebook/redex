/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SwitchEquivPrerequisites.h"

#include "ConstantEnvironment.h"
#include "SwitchEquivFinder.h"

std::unique_ptr<SwitchEquivFinder> create_switch_equiv_finder(
    cfg::ControlFlowGraph* cfg,
    size_t leaf_dup_threshold,
    SwitchEquivFinder::DuplicateCaseStrategy duplicates_strategy,
    std::vector<cfg::Block*>* out_prologue_blocks) {
  // Step 1: Gather linear prologue blocks to find the first branch point
  std::vector<cfg::Block*> prologue_blocks;
  if (!gather_linear_prologue_blocks(cfg, &prologue_blocks)) {
    return nullptr;
  }

  // Step 2: Run constant propagation analysis
  auto fixpoint =
      std::make_shared<constant_propagation::intraprocedural::FixpointIterator>(
          /* cp_state */ nullptr, *cfg, SwitchEquivFinder::Analyzer());
  fixpoint->run(ConstantEnvironment());

  // Step 3: Find the determining register (the one being switched on)
  reg_t determining_reg;
  // Array bound for prologue_blocks checked in gather_linear_prologue_blocks
  // NOLINTNEXTLINE
  if (!find_determining_reg(*fixpoint, prologue_blocks.back(),
                            &determining_reg)) {
    return nullptr;
  }

  // Step 4: Get the root branch instruction
  auto* last_prologue_block = prologue_blocks.back();
  auto last_prologue_insn = last_prologue_block->get_last_insn();
  auto root_branch = cfg->find_insn(last_prologue_insn->insn);

  // Before returning, output prologue blocks if requested
  if (out_prologue_blocks != nullptr) {
    *out_prologue_blocks = std::move(prologue_blocks);
  }

  // Step 5: Construct and return SwitchEquivFinder
  return std::make_unique<SwitchEquivFinder>(cfg, root_branch, determining_reg,
                                             leaf_dup_threshold, fixpoint,
                                             duplicates_strategy);
}
