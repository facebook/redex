/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <vector>

#include "IRList.h"
#include "Pass.h"

class BranchPrefixHoistingPass : public Pass {
 public:
  BranchPrefixHoistingPass() : Pass("BranchPrefixHoistingPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static int process_code(IRCode*);
  static int process_cfg(cfg::ControlFlowGraph&);
  static int process_hoisting_for_block(cfg::Block*, cfg::ControlFlowGraph&);

  static bool has_side_effect_on_vregs(const IRInstruction&,
                                       const std::unordered_set<uint16_t>&);

  static boost::optional<IRInstruction> get_next_common_insn(
      std::vector<IRList::iterator>, const std::vector<cfg::Block*>&, int);

 private:
  static bool is_block_eligible(cfg::Block*);
  static bool is_insn_eligible(const IRInstruction& insn);

  static void hoist_insns_for_block(
      cfg::Block* block,
      const IRList::iterator& pos,
      const std::vector<cfg::Block*>& succ_blocks,
      cfg::ControlFlowGraph& cfg,
      const std::vector<IRInstruction>& insns_to_hoist);

  static std::vector<IRInstruction> get_insns_to_hoist(
      const std::vector<cfg::Block*>& succ_blocks,
      const std::unordered_set<uint16_t>& crit_regs);
};
