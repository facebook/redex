/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>
#include <vector>

#include "ConstantUses.h"
#include "IRList.h"
#include "Pass.h"
#include "TypeInference.h"

class IRCode;

namespace cfg {
class Block;
class ControlFlowGraph;
} // namespace cfg

class BranchPrefixHoistingPass : public Pass {
 public:
  BranchPrefixHoistingPass() : Pass("BranchPrefixHoistingPass") {}

  void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;

  static int process_code(IRCode*, DexMethod*);
  static int process_cfg(cfg::ControlFlowGraph&,
                         type_inference::TypeInference&,
                         constant_uses::ConstantUses&);
  static int process_hoisting_for_block(cfg::Block*,
                                        cfg::ControlFlowGraph&,
                                        type_inference::TypeInference&,
                                        constant_uses::ConstantUses&);

  static void setup_side_effect_on_vregs(const IRInstruction&,
                                         std::unordered_map<reg_t, bool>&);

  static boost::optional<IRInstruction> get_next_common_insn(
      std::vector<IRList::iterator>,
      const std::vector<cfg::Block*>&,
      int,
      constant_uses::ConstantUses&);

 private:
  static bool is_block_eligible(cfg::Block*);
  static bool is_insn_eligible(const IRInstruction& insn);

  static size_t hoist_insns_for_block(
      cfg::Block* block,
      const IRList::iterator& pos,
      const std::vector<cfg::Block*>& succ_blocks,
      cfg::ControlFlowGraph& cfg,
      const std::vector<IRInstruction>& insns_to_hoist,
      const std::unordered_map<reg_t, bool>& crit_regs,
      type_inference::TypeInference& type_inference);

  static std::vector<IRInstruction> get_insns_to_hoist(
      const std::vector<cfg::Block*>& succ_blocks,
      std::unordered_map<reg_t, bool>& crit_regs,
      constant_uses::ConstantUses& constant_uses);
  static bool create_move_and_fix_clobbered(
      const IRList::iterator& pos,
      std::vector<IRInstruction*>& heap_insn_objs,
      cfg::Block* block,
      cfg::ControlFlowGraph& cfg,
      const std::unordered_map<reg_t, bool>& crit_regs,
      type_inference::TypeInference& type_inference);

  static void skip_pos_debug(IRList::iterator& it, const IRList::iterator& end);
};
