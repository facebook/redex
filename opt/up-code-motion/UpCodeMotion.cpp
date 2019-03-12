/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass eliminates gotos by moving trivial const-instructions
 * before a conditional branch.
 *
 * For example:
 *
 *       IF_EQZ v2, L1
 *       CONST v0, 1
 *       ... (GOTO elsewhere or RETURN or THROW)
 *   L1: CONST v0, 0 // where L1 is only reachable via the above IF-instruction
 *       GOTO L2
 *
 * becomes
 *
 *       CONST v0, 0
 *       IF_EQZ v2, L2
 *       CONST v0, 1
 *       ...
 *
 */

#include "UpCodeMotion.h"

#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_INSTRUCTIONS_MOVED = "num_instructions_moved";
constexpr const char* METRIC_BRANCHES_MOVED_OVER = "num_branches_moved_over";
constexpr const char* METRIC_INVERTED_CONDITIONAL_BRANCHES =
    "num_inverted_conditional_branches";

} // namespace

UpCodeMotionPass::Stats UpCodeMotionPass::process_code(IRCode* code) {
  Stats stats;

  code->build_cfg(/* editable = true*/);
  auto& cfg = code->cfg();

  // Helper function that scans a block for leading const instructions,
  // and returning a value that indicates whether there's no other kind of
  // instruction in the block.
  auto gather_movable_instructions =
      [](cfg::Block* b,
         std::unordered_map<uint32_t, IRInstruction*>* instructions) {
        for (auto it = b->begin(); it != b->end(); it++) {
          const MethodItemEntry& mie = *it;
          if (mie.type != MFLOW_OPCODE) {
            continue;
          }

          // We really only support const at this time;
          // not const-wide, cost-class, or const-string.
          IRInstruction* insn = mie.insn;
          if (insn->opcode() != OPCODE_CONST) {
            return false;
          }

          instructions->emplace(insn->dest(), insn);
        }

        return true;
      };

  std::unordered_set<cfg::Block*> blocks_to_remove;
  for (cfg::Block* b : cfg.blocks()) {
    if (blocks_to_remove.count(b)) {
      continue;
    }

    auto br = b->branchingness();
    if (br != opcode::BRANCH_IF) {
      continue;
    }

    auto last_insn_it = b->get_last_insn();
    always_assert(last_insn_it != b->end());

    auto if_insn = last_insn_it->insn;
    always_assert(is_conditional_branch(if_insn->opcode()));
    always_assert(!if_insn->is_wide());

    // We found a block that ends with a conditional branch.
    // Let's see if our transformation can be applied.
    cfg::Edge* branch_edge = cfg.get_succ_edge_of_type(b, cfg::EDGE_BRANCH);
    always_assert(branch_edge != nullptr);
    cfg::Edge* goto_edge = cfg.get_succ_edge_of_type(b, cfg::EDGE_GOTO);
    always_assert(goto_edge != nullptr);

    // Helper function that, given a branch and a goto edge, figures out if all
    // movable const-instructions of the branch edge target block have a
    // matching (same register) leading const instruction in the goto edge
    // target block.
    auto gather_instructions_to_insert =
        [gather_movable_instructions,
         if_insn](cfg::Edge* branch_edge, cfg::Edge* goto_edge,
                  std::vector<IRInstruction*>* instructions_to_insert) {
          cfg::Block* branch_block = branch_edge->target();
          // The branch edge target block must end in a goto, and
          // have a unique predecessor.
          if (branch_block->branchingness() != opcode::BRANCH_GOTO ||
              branch_block->preds().size() != 1) {
            TRACE(UCM, 5, "[up code motion] giving up: branch block\n");
            return false;
          }

          std::unordered_map<uint32_t, IRInstruction*> branch_instructions;
          // Gather all of the const instructions of the branch edge
          // target block; give up when there are any other instructions.
          if (!gather_movable_instructions(branch_block,
                                           &branch_instructions)) {
            TRACE(UCM, 5, "[up code motion] giving up: gather\n");
            return false;
          }

          cfg::Block* goto_block = goto_edge->target();
          std::unordered_map<uint32_t, IRInstruction*> goto_instructions;

          // Gather all of the const instructions of the branch edge
          // goto block; it's okay if there are other trailing instructions.
          gather_movable_instructions(goto_block, &goto_instructions);

          // In the following, we check if all the registers assigned to by
          // const instructions of the branch edge target block also
          // get assigned by the goto edge target block, and are not used by the
          // if-instruction.
          if (goto_instructions.size() < branch_instructions.size()) {
            TRACE(UCM, 5, "[up code motion] giving up: instructions.size()\n");
            return false;
          }

          bool missing_dest = false;
          bool if_reg_clobber = false;
          std::vector<uint32_t> dests;
          for (auto& p : branch_instructions) {
            uint32_t dest = p.first;
            dests.push_back(dest);
            if (goto_instructions.count(dest) == 0) {
              missing_dest = true;
            }
            for (auto reg : if_insn->srcs()) {
              if (reg == dest) {
                if_reg_clobber = true;
              }
            }
          }
          if (missing_dest) {
            TRACE(UCM, 5, "[up code motion] giving up: missing dest\n");
            return false;
          }
          if (if_reg_clobber) {
            TRACE(UCM, 5, "[up code motion] giving up: if reg clobber`\n");
            return false;
          }

          // We sort registers to make things determistic.
          std::sort(dests.begin(), dests.end());
          for (uint32_t dest : dests) {
            IRInstruction* insn = branch_instructions.at(dest);
            insn = new IRInstruction(*insn);
            instructions_to_insert->push_back(insn);
          }

          return true;
        };

    std::vector<IRInstruction*> instructions_to_insert;
    // Can we do our code transformation directly?
    if (!gather_instructions_to_insert(branch_edge, goto_edge,
                                       &instructions_to_insert)) {
      // Or do we first have to flip the conditional branch?
      if (!gather_instructions_to_insert(goto_edge, branch_edge,
                                         &instructions_to_insert)) {
        // We just can't do it.
        continue;
      }

      // Flip conditional branch before doing actual transformation.
      if_insn->set_opcode(opcode::invert_conditional_branch(if_insn->opcode()));
      // swap goto and branch target
      cfg::Block* branch_target = branch_edge->target();
      cfg::Block* goto_target = goto_edge->target();
      cfg.set_edge_target(branch_edge, goto_target);
      cfg.set_edge_target(goto_edge, branch_target);
      stats.inverted_conditional_branches++;
    }

    // Okay, we can apply our transformation:
    // We insert the (cloned) const instructions of the branch edge target block
    // just in front of the if-instruction.
    // And then we remove the branch edge target block, rewiring the branch edge
    // to point to the goto target of the branch edge target block.

    cfg::Block* branch_block = branch_edge->target();
    for (IRInstruction* insn : instructions_to_insert) {
      auto it = b->to_cfg_instruction_iterator(last_insn_it);
      cfg.insert_before(it, insn);
    }
    cfg.set_edge_target(
        branch_edge,
        cfg.get_succ_edge_of_type(branch_block, cfg::EDGE_GOTO)->target());
    blocks_to_remove.insert(branch_block);

    stats.instructions_moved += instructions_to_insert.size();
    stats.branches_moved_over++;
  }

  for (cfg::Block* b : blocks_to_remove) {
    cfg.remove_block(b);
  }

  code->clear_cfg();
  return stats;
}

void UpCodeMotionPass::run_pass(DexStoresVector& stores,
                                ConfigFiles& /* unused */,
                                PassManager& mgr) {
  auto scope = build_class_scope(stores);

  Stats stats = walk::parallel::reduce_methods<Stats>(
      scope,
      [](DexMethod* method) -> Stats {
        const auto code = method->get_code();
        if (!code) {
          return Stats{};
        }

        Stats stats = UpCodeMotionPass::process_code(code);
        if (stats.instructions_moved || stats.branches_moved_over) {
          TRACE(UCM, 3,
                "[up code motion] Moved %u instructions over %u conditional "
                "branches while inverting %u conditional branches in {%s}\n",
                stats.instructions_moved, stats.branches_moved_over,
                stats.inverted_conditional_branches, SHOW(method));
        }
        return stats;
      },
      [](Stats a, Stats b) {
        Stats c;
        c.instructions_moved = a.instructions_moved + b.instructions_moved;
        c.branches_moved_over = a.branches_moved_over + b.branches_moved_over;
        c.inverted_conditional_branches =
            a.inverted_conditional_branches + b.inverted_conditional_branches;
        return c;
      });

  mgr.incr_metric(METRIC_INSTRUCTIONS_MOVED, stats.instructions_moved);
  mgr.incr_metric(METRIC_BRANCHES_MOVED_OVER, stats.branches_moved_over);
  mgr.incr_metric(METRIC_INVERTED_CONDITIONAL_BRANCHES,
                  stats.inverted_conditional_branches);
  TRACE(UCM, 1,
        "[up code motion] Moved %u instructions over "
        "%u conditional branches while inverting %u conditional branches in "
        "total\n",
        stats.instructions_moved, stats.branches_moved_over,
        stats.inverted_conditional_branches);
}

static UpCodeMotionPass s_pass;
