/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * This pass looks for repeated if else statements comparing the same value
 * against int constants, arising due to inlining of structured R values
 * and converts them into switch statements
 */

#include "IntroduceSwitch.h"

#include <algorithm>
#include <vector>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "SwitchEquivFinder.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_SWITCH_INTRODUCED = "num_switch_introduced";
constexpr const char* METRIC_SWITCH_CASES = "num_switch_introduced_cases";
constexpr const char* METRIC_COMPACT_SWITCHES = "num_packed_switches";
constexpr const char* METRIC_SPARSE_SWITCHES = "num_sparse_switches";
constexpr const char* METRIC_INSTRUCTIONS_REMOVED = "num_instructions_removed";
constexpr const char* METRIC_INSTRUCTIONS_ADDED = "num_instructions_added";

constexpr const int MIN_SWITCH_LENGTH = 3;

class IntroduceSwitch {
 private:
  /* Determines if a block ends with an if where that could be a candidate
   * for a nested if else chain that is like a switch.
   * If not, nullptr is returned, if so the branching instruction is returned
   * along with the register being tested.
   *
   * Generally experimental runs indicate that an if with three cases has fewer
   * instructions when represented as an if, so that is the current default
   *
   * It is also the case from experimental evidence in prior work, that compact
   * switches run faster than nested if/else and that the majority of other
   * switch statements also out perform nested if in cases where the if is not
   * significantly hand-optimized to select the most common case early.
   * It can also be beneficial to performance to pad out a switch into compact,
   * but the tradeoffs in code size are different and better handled separately.
   */
  static std::pair<IRInstruction*, uint32_t> candidate_switch_start(
      cfg::Block* block) {
    // TODO make this more general
    IRInstruction *const_set = nullptr, *if_ins = nullptr;
    for (auto it = block->rbegin(); it != block->rend(); it++) {
      if (it->type == MFLOW_OPCODE && (it->insn->opcode() == OPCODE_IF_NE ||
                                       it->insn->opcode() == OPCODE_IF_EQ)) {
        if_ins = it->insn;
        it++;
        if (it != block->rend() && it->type == MFLOW_OPCODE &&
            it->insn->opcode() == OPCODE_CONST) {
          const_set = it->insn;
          break;
        }
      }
      break;
    }
    if (const_set != nullptr && if_ins != nullptr) {
      auto compare_dest = const_set->dest();
      auto compare_srcs = if_ins->srcs();
      always_assert(compare_srcs.size() == 2);

      // Is constant compared in if, if so other one is potential switch reg
      size_t shared_index = compare_srcs[0] == compare_dest
                                ? 0
                                : (compare_srcs[1] == compare_dest ? 1 : 2);
      if (shared_index != 2) {
        return {if_ins, compare_srcs[!shared_index]};
      }
    }
    return {nullptr, 0};
  }

  static IntroduceSwitchPass::Metrics merge_blocks(cfg::ControlFlowGraph& cfg) {
    std::unordered_set<cfg::Block*> visited_blocks;

    int32_t intro_switch = 0;
    int32_t switch_cases = 0;
    int32_t sparse = 0;
    int32_t num_compact = 0;

    for (cfg::Block* block : cfg.blocks()) {
      if (visited_blocks.count(block) == 0) {
        visited_blocks.insert(block);
        auto possible_start_block = candidate_switch_start(block);
        if (possible_start_block.first != nullptr) {
          const cfg::InstructionIterator& iter =
              block->to_cfg_instruction_iterator(block->get_last_insn());
          SwitchEquivFinder* finder =
              new SwitchEquivFinder(&cfg, iter, possible_start_block.second);
          if (finder->success()) {
            for (const auto& b : finder->visited_blocks()) {
              visited_blocks.insert(b);
            }
            auto key_to_case = finder->key_to_case();

            // if the chain is too small, there's no benefit in compacting it
            if (key_to_case.size() >= MIN_SWITCH_LENGTH) {
              TRACE(INTRO_SWITCH, 3, "Found switch-like chain: { ");
              for (const auto& b : key_to_case) {
                TRACE(INTRO_SWITCH, 3, "%d ", b.second->id());
              }
              TRACE(INTRO_SWITCH, 3, "}\n");
              intro_switch++;

              bool compact = can_be_compact(key_to_case);
              if (compact) {
                num_compact++;
              } else {
                sparse++;
              }
              IRInstruction* new_switch = new IRInstruction(
                  compact ? OPCODE_PACKED_SWITCH : OPCODE_SPARSE_SWITCH);
              new_switch->set_src(0, possible_start_block.second);
              std::vector<std::pair<int32_t, cfg::Block*>> edges;
              cfg::Block* default_block = nullptr;

              const auto& extra_loads = finder->extra_loads();
              for (const auto& e : key_to_case) {
                const auto& key = e.first;
                cfg::Block* case_block = e.second;
                if (key) {
                  edges.emplace_back(*key, case_block);
                  switch_cases++;
                } else if (default_block == nullptr) {
                  default_block = case_block;
                } else {
                  // Possibly dead code
                  continue;
                }
                // Finder has identified potentially necessary register loads
                // for each block. This extracts and adds them
                // We rely on DCE to remove truely redundant ones
                const auto& needed_loads = extra_loads.find(case_block);
                if (needed_loads != extra_loads.end()) {
                  for (const auto& register_and_insn : needed_loads->second) {
                    IRInstruction* load_insn = register_and_insn.second;
                    if (load_insn != nullptr) {
                      // null signifies the upper half of a wide load.
                      auto copy = new IRInstruction(*load_insn);
                      case_block->push_front(copy);
                    }
                  }
                }
              }

              for (auto it = block->begin(); it != block->end(); it++) {
                if (it->insn == possible_start_block.first) {
                  // this also deletes the outgoing branch edges
                  block->remove_insn(it);
                  break;
                }
              }
              cfg.create_branch(block, new_switch, default_block, edges);
            }
          }
        }
      }
    }
    IntroduceSwitchPass::Metrics m = IntroduceSwitchPass::Metrics();
    m.switch_intro = intro_switch;
    m.switch_cases = switch_cases;
    m.compact_switch = num_compact;
    m.sparse_switch = sparse;
    return m;
  }

  // Determine if this is a compact or non-compact switch
  static bool can_be_compact(const SwitchEquivFinder::KeyToCase& key_to_case) {
    bool last_case_set = false;
    bool compact_direction_set = false;
    int32_t last_case = 0;
    bool compact_check_positive = false;

    for (const auto& pair : key_to_case) {
      const auto& key = pair.first;
      cfg::Block* case_block = pair.second;
      if (key) {
        if (!last_case_set) {
          last_case_set = true;
          last_case = *key;
        } else {
          auto difference = last_case - *key;
          last_case = *key;
          if (std::abs(difference) == 1) {
            if (!compact_direction_set) {
              compact_direction_set = true;
              compact_check_positive = difference > 0;
            } else {
              if (!((compact_check_positive && difference > 0) ||
                    (!compact_check_positive && difference < 0))) {
                return false;
              }
            }
          } else {
            return false;
          }
        }
      }
    }
    return true;
  }

 public:
  static IntroduceSwitchPass::Metrics process_method(DexMethod* method) {
    auto code = method->get_code();

    TRACE(INTRO_SWITCH, 4, "Class: %s\n", SHOW(method->get_class()));
    TRACE(INTRO_SWITCH, 3, "Method: %s\n", SHOW(method->get_name()));
    auto init_opcode_count = code->count_opcodes();
    TRACE(INTRO_SWITCH, 4, "Initial opcode count: %d\n", init_opcode_count);

    TRACE(INTRO_SWITCH, 3, "input code\n%s", SHOW(code));
    code->build_cfg(/* editable */ true);
    auto& cfg = code->cfg();

    TRACE(INTRO_SWITCH, 3, "before %s\n", SHOW(cfg));

    IntroduceSwitchPass::Metrics switch_metrics = merge_blocks(cfg);

    code->clear_cfg();

    if (switch_metrics.switch_intro > 0) {
      TRACE(INTRO_SWITCH,
            3,
            "%d blocks transformed\n",
            switch_metrics.switch_cases);
      TRACE(INTRO_SWITCH, 3, "after %s\n", SHOW(cfg));
      TRACE(INTRO_SWITCH, 5, "Opcode count: %d\n", init_opcode_count);

      auto final_opcode_count = code->count_opcodes();
      int32_t opcode_difference = init_opcode_count - final_opcode_count;

      if (opcode_difference < 0 && switch_metrics.switch_intro > 0) {
        TRACE(INTRO_SWITCH,
              3,
              "method %s got larger: (%d -> %d)\n",
              SHOW(method),
              init_opcode_count,
              final_opcode_count);
        switch_metrics.added_instrs = abs(opcode_difference);
      } else if (switch_metrics.switch_intro > 0) {
        switch_metrics.removed_instrs = opcode_difference;
      }
      TRACE(INTRO_SWITCH, 4, "Final opcode count: %d\n", code->count_opcodes());
      TRACE(INTRO_SWITCH, 3, "output code\n%s", SHOW(code));

      return switch_metrics;
    }
    return IntroduceSwitchPass::Metrics();
  }
};
} // namespace

IntroduceSwitchPass::Metrics IntroduceSwitchPass::run(DexMethod* method) {
  return IntroduceSwitch::process_method(method);
}

void IntroduceSwitchPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& /* unused */,
                                   PassManager& mgr) {
  auto scope = build_class_scope(stores);

  Metrics total_switch_cases = walk::parallel::reduce_methods<Metrics>(
      scope,
      [](DexMethod* m) -> Metrics {
        if (!m->get_code()) {
          return Metrics();
        }
        return IntroduceSwitch::process_method(m);
      },
      [](Metrics a, Metrics b) { return a + b; });

  if (total_switch_cases.switch_intro > 0) {
    mgr.incr_metric(METRIC_SWITCH_INTRODUCED, total_switch_cases.switch_intro);
    mgr.incr_metric(METRIC_SWITCH_CASES, total_switch_cases.switch_cases);
    mgr.incr_metric(METRIC_SPARSE_SWITCHES, total_switch_cases.sparse_switch);
    mgr.incr_metric(METRIC_COMPACT_SWITCHES, total_switch_cases.compact_switch);
    mgr.incr_metric(METRIC_INSTRUCTIONS_REMOVED,
                    total_switch_cases.removed_instrs);
    mgr.incr_metric(METRIC_INSTRUCTIONS_ADDED, total_switch_cases.added_instrs);

    TRACE(INTRO_SWITCH,
          1,
          "Number of nested if elses converted to switches: %d\n",
          total_switch_cases.switch_cases);
  }
}

static IntroduceSwitchPass s_pass;
