/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "StringBuilderAppendChain.h"

#include <algorithm>
#include <optional>
#include <string>
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
#include "MethodUtil.h"
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

// Whether `insn` invokes the String overload of StringBuilder.append. The other
// overloads take a value whose text is a conversion, so they are not
// interchangeable with it.
bool is_string_append(const IRInstruction* insn) {
  return insn->has_method() &&
         insn->get_method() == method::java_lang_StringBuilder_append_String();
}

} // namespace

size_t reduce_two_append_concats(
    const intraprocedural::FixpointIterator& fp_iter,
    cfg::ControlFlowGraph& cfg) {
  // Scanning for toString() first keeps the more expensive fixpoint off methods
  // that have no builder to reduce.
  auto tostring_instructions =
      stringbuilder_analysis::find_tostring_instructions(
          cfg, method::java_lang_StringBuilder_toString());
  if (tostring_instructions.empty()) {
    return 0;
  }
  auto tostring_to_state =
      stringbuilder_analysis::gather_builder_states(cfg, tostring_instructions);

  struct TwoAppendInsns {
    const IRInstruction* tostring;
    const IRInstruction* append_a;
    const IRInstruction* append_b;
  };
  std::vector<TwoAppendInsns> candidates;
  for (const auto& [tostring_insn, state] : tostring_to_state) {
    // String.concat takes a String receiver and argument, so appends of the
    // other modeled types are not candidates.
    if (state.size() == 2 && is_string_append(state[0]) &&
        is_string_append(state[1])) {
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
      if (is_string_append(insn) && is_known_non_null(env.get(insn->src(1)))) {
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

size_t merge_adjacent_constant_appends(
    const intraprocedural::FixpointIterator& fp_iter,
    cfg::ControlFlowGraph& cfg) {
  // Scanning for toString() first keeps the more expensive fixpoint off methods
  // that have no builder to merge.
  auto tostring_instructions =
      stringbuilder_analysis::find_tostring_instructions(
          cfg, method::java_lang_StringBuilder_toString());
  if (tostring_instructions.empty()) {
    return 0;
  }
  auto tostring_to_state =
      stringbuilder_analysis::gather_builder_states(cfg, tostring_instructions);

  // The StringBuilder(String) constructor carries a String too, but is never
  // merged or removed, so it breaks a sequence of appends.

  // The fixpoint replay below walks every instruction, so it is worth entering
  // only when some modeled builder appends a String at all.
  if (std::none_of(tostring_to_state.begin(), tostring_to_state.end(),
                   [&](const auto& entry) {
                     const auto& state = entry.second;
                     return std::any_of(state.begin(), state.end(),
                                        is_string_append);
                   })) {
    return 0;
  }

  UnorderedMap<const IRInstruction*, const DexString*>
      append_invocation_to_constant;
  // The self-loop pattern `R = R.append(...)`: the move-result writes the
  // receiver back, or there is no move-result.
  UnorderedSet<const IRInstruction*> self_loop_appends;
  for (auto* block : cfg.blocks()) {
    auto env = fp_iter.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    auto last_insn = block->get_last_insn();
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (is_string_append(insn)) {
        auto result_it =
            cfg.move_result_of(block->to_cfg_instruction_iterator(mie));
        if (result_it.is_end() || result_it->insn->dest() == insn->src(0)) {
          self_loop_appends.insert(insn);
        }
        auto value = env.get(insn->src(1));
        if (const auto& sd = value.maybe_get<StringDomain>()) {
          if (auto c = sd->get_constant()) {
            append_invocation_to_constant.emplace(insn, *c);
          }
        }
      }
      fp_iter.analyze_instruction(insn, &env, insn == last_insn->insn);
    }
  }

  auto is_mergeable = [&](const IRInstruction* op) {
    return is_string_append(op) &&
           append_invocation_to_constant.count(op) != 0u &&
           self_loop_appends.contains(op);
  };

  // Which toString()s each append contributes to, as indices into
  // `tostring_to_state`. Recorded in state order, so two appends reaching the
  // same calls compare equal as vectors.
  UnorderedMap<const IRInstruction*, std::vector<size_t>>
      append_invocation_to_tostrings;
  for (size_t index = 0; index < tostring_to_state.size(); ++index) {
    for (const auto* op : tostring_to_state[index].second) {
      append_invocation_to_tostrings[op].push_back(index);
    }
  }

  // A group is two or more consecutive ops that are all mergeable, all take
  // the builder from the same register and all contribute to the same
  // toString()s. It becomes one append: the first op's operand is replaced by
  // the concatenation of the group's constants, and the rest are deleted.
  UnorderedMap<const IRInstruction*, const DexString*>
      first_append_invocation_to_concatenation;
  UnorderedSet<const IRInstruction*> append_invocations_to_drop;
  UnorderedSet<const IRInstruction*> merged_append_invocations;
  size_t merged_away = 0;
  auto find_mergeable_append_end = [&is_mergeable,
                                    &append_invocation_to_tostrings](
                                       const auto& state, size_t begin) {
    reg_t r = state[begin]->src(0);
    const auto& tostrings = append_invocation_to_tostrings.at(state[begin]);
    for (size_t end = begin + 1; end < state.size(); ++end) {
      if (!is_mergeable(state[end]) || state[end]->src(0) != r ||
          append_invocation_to_tostrings.at(state[end]) != tostrings) {
        return end;
      }
    }
    return state.size();
  };
  for (const auto& [tostring_insn, state] : tostring_to_state) {
    for (size_t begin = 0; begin < state.size();) {
      if (!is_mergeable(state[begin])) {
        ++begin;
        continue;
      }
      size_t end = find_mergeable_append_end(state, begin);
      if (end - begin >= 2) {
        // Overlapping an already recorded merge would merge the same append
        // twice.
        if (std::none_of(state.data() + begin, state.data() + end,
                         [&merged_append_invocations](const auto* op) {
                           return merged_append_invocations.contains(op);
                         })) {
          std::string concatenation;
          for (size_t pos = begin; pos < end; ++pos) {
            concatenation +=
                append_invocation_to_constant.at(state[pos])->str_copy();
            merged_append_invocations.insert(state[pos]);
            if (pos > begin) {
              append_invocations_to_drop.insert(state[pos]);
            }
          }
          first_append_invocation_to_concatenation.emplace(
              state[begin], DexString::make_string(concatenation));
          merged_away += end - begin - 1;
        }
      }
      begin = end;
    }
  }
  if (first_append_invocation_to_concatenation.empty()) {
    return 0;
  }

  // `mutation.remove` takes each dropped append's move-result-object with it.
  // The operand loads the merge orphans are left for a later LocalDce run.
  cfg::CFGMutation mutation(cfg);
  for (auto* block : cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      auto it = block->to_cfg_instruction_iterator(mie);
      if (auto keep_it = first_append_invocation_to_concatenation.find(insn);
          keep_it != first_append_invocation_to_concatenation.end()) {
        reg_t reg = cfg.allocate_temp();
        auto* const_insn = (new IRInstruction(OPCODE_CONST_STRING))
                               ->set_string(keep_it->second);
        auto* move_insn = (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                              ->set_dest(reg);
        mutation.insert_before(it, {const_insn, move_insn});
        insn->set_src(1, reg);
      }
      if (append_invocations_to_drop.contains(insn)) {
        mutation.remove(it);
      }
    }
  }
  mutation.flush();

  return merged_away;
}

size_t replace_constant_tostring_with_const_string(
    const intraprocedural::FixpointIterator& fp_iter,
    cfg::ControlFlowGraph& cfg) {
  auto tostring_instructions =
      stringbuilder_analysis::find_tostring_instructions(
          cfg, method::java_lang_StringBuilder_toString());
  if (tostring_instructions.empty()) {
    return 0;
  }
  auto tostring_to_state =
      stringbuilder_analysis::gather_builder_states(cfg, tostring_instructions);

  UnorderedMap<const IRInstruction*, const IRInstruction*> tostring_to_append;
  for (const auto& [tostring_insn, state] : tostring_to_state) {
    // We only need to handle chains with single append because
    // merge_adjacent_constant_appends should have merged consecutive appends on
    // constants.
    if (state.size() == 1u && is_string_append(state[0])) {
      tostring_to_append.emplace(tostring_insn, state[0]);
    }
  }

  if (tostring_to_append.empty()) {
    return 0;
  }

  UnorderedMap<const IRInstruction*, const DexString*>
      append_invocation_to_constant;
  for (auto* block : cfg.blocks()) {
    auto env = fp_iter.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    auto last_insn = block->get_last_insn();
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      if (is_string_append(insn)) {
        auto value = env.get(insn->src(1));
        if (const auto& sd = value.maybe_get<StringDomain>()) {
          if (auto c = sd->get_constant()) {
            append_invocation_to_constant.emplace(insn, *c);
          }
        }
      }
      fp_iter.analyze_instruction(insn, &env, insn == last_insn->insn);
    }
  }

  size_t replaced = 0;
  cfg::CFGMutation mutation(cfg);
  for (auto* block : cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto* insn = mie.insn;
      const auto append_it = tostring_to_append.find(insn);
      if (append_it == tostring_to_append.end()) {
        continue;
      }
      auto constant_string_it =
          append_invocation_to_constant.find(append_it->second);
      if (constant_string_it == append_invocation_to_constant.end()) {
        continue;
      }
      auto result_it =
          cfg.move_result_of(block->to_cfg_instruction_iterator(mie));
      // No need to handle a toString() whose result is discarded. A dead code
      // elimination pass will delete it.
      if (result_it.is_end()) {
        continue;
      }
      auto* const_insn = (new IRInstruction(OPCODE_CONST_STRING))
                             ->set_string(constant_string_it->second);
      // Replacing the toString() removes its move-result-object too, so this
      // must write the same register.
      auto* move_insn = (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                            ->set_dest(result_it->insn->dest());
      mutation.replace(block->to_cfg_instruction_iterator(mie),
                       {const_insn, move_insn});
      replaced++;
    }
  }
  mutation.flush();

  return replaced;
}

} // namespace constant_propagation::stringbuilder_append_chain
