/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringBuilderAppendChain.h"

#include <utility>
#include <vector>

#include "CFGMutation.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "ControlFlow.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "StringBuilderAnalysis.h"

namespace constant_propagation::stringbuilder_append_chain {

namespace {

// The concat reads its operands at the toString(), by which point the register
// an append read may hold something else:
//
//   append(v0, v1)          ; the builder gets v1's value
//   const-string "x" -> v1
//   toString(v0)            ; a concat reading v1 would see "x"
//
// Inserting `move-object vFresh, v1` right before the append copies the value
// into a register no other instruction writes to, so it still reads back at
// the toString(); the concat takes vFresh as its operand. A builder reached by
// more than one toString() reuses the temp already allocated for that append,
// so `capture_moves` records the move made for each append.
reg_t capture_append_value(
    cfg::ControlFlowGraph& cfg,
    UnorderedMap<const IRInstruction*, IRInstruction*>& capture_moves,
    const IRInstruction* append_insn) {
  if (auto move_it = capture_moves.find(append_insn);
      move_it != capture_moves.end()) {
    return move_it->second->dest();
  }
  reg_t reg = cfg.allocate_temp();
  auto* move = (new IRInstruction(OPCODE_MOVE_OBJECT))
                   ->set_src(0, append_insn->src(1))
                   ->set_dest(reg);
  capture_moves.emplace(append_insn, move);
  return reg;
}

} // namespace

size_t reduce_two_append_concats(
    const intraprocedural::FixpointIterator& fp_iter,
    cfg::ControlFlowGraph& cfg) {
  // Scanning for toString() first keeps the more expensive fixpoint off methods
  // that have no builder to reduce.
  auto tostring_instructions =
      stringbuilder_analysis::find_tostring_instructions(
          cfg,
          DexMethod::get_method(
              "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;"));
  if (tostring_instructions.empty()) {
    return 0;
  }
  auto tostring_to_state =
      stringbuilder_analysis::gather_builder_states(cfg, tostring_instructions);

  // String.concat takes a String receiver and argument, so appends of the other
  // modeled types are not candidates.
  const auto* append_string = DexMethod::get_method(
      "Ljava/lang/StringBuilder;.append:(Ljava/lang/String;)Ljava/lang/"
      "StringBuilder;");

  struct TwoAppendInsns {
    const IRInstruction* tostring;
    const IRInstruction* append_a;
    const IRInstruction* append_b;
  };
  std::vector<TwoAppendInsns> candidates;
  for (const auto& [tostring_insn, state] : tostring_to_state) {
    if (state.size() == 2 && state[0]->get_method() == append_string &&
        state[1]->get_method() == append_string) {
      candidates.push_back({tostring_insn, state[0], state[1]});
    }
  }
  if (candidates.empty()) {
    return 0;
  }

  // Read each append operand's non-nullness from the solved fixpoint by
  // replaying it per block to recover the state before each append.
  UnorderedSet<const IRInstruction*> non_null_ops;
  for (auto* block : cfg.blocks()) {
    auto env = fp_iter.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    auto last_insn = block->get_last_insn();
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (insn->has_method() && insn->get_method() == append_string &&
          is_known_non_null(env.get(insn->src(1)))) {
        non_null_ops.insert(insn);
      }
      fp_iter.analyze_instruction(insn, &env, insn == last_insn->insn);
    }
  }

  UnorderedMap<const IRInstruction*, IRInstruction*> capture_moves_to_insert;
  UnorderedMap<const IRInstruction*, std::pair<reg_t, reg_t>>
      tostring_to_concat_srcs;
  auto* concat_ref = DexMethod::make_method(
      "Ljava/lang/String;.concat:(Ljava/lang/String;)Ljava/lang/String;");

  size_t reduced = 0;
  for (const auto& cand : candidates) {
    if (!non_null_ops.contains(cand.append_a) ||
        !non_null_ops.contains(cand.append_b)) {
      continue;
    }
    reg_t reg_a =
        capture_append_value(cfg, capture_moves_to_insert, cand.append_a);
    reg_t reg_b =
        capture_append_value(cfg, capture_moves_to_insert, cand.append_b);
    tostring_to_concat_srcs.emplace(cand.tostring, std::pair(reg_a, reg_b));
    reduced++;
  }

  if (tostring_to_concat_srcs.empty()) {
    return 0;
  }

  // Insert each capture move before its append, and turn each toString() into
  // String.concat.
  cfg::CFGMutation mutation(cfg);
  for (auto* block : cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      // insert moves
      if (auto move_it = capture_moves_to_insert.find(insn);
          move_it != capture_moves_to_insert.end()) {
        mutation.insert_before(block->to_cfg_instruction_iterator(mie),
                               {move_it->second});
      }
      // replace toString with concat
      if (auto srcs_it = tostring_to_concat_srcs.find(insn);
          srcs_it != tostring_to_concat_srcs.end()) {
        insn->set_method(concat_ref)->set_srcs_size(2);
        insn->set_src(0, srcs_it->second.first);
        insn->set_src(1, srcs_it->second.second);
      }
    }
  }
  mutation.flush();

  return reduced;
}

} // namespace constant_propagation::stringbuilder_append_chain
