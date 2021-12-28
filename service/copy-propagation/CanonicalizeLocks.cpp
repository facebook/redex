/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CanonicalizeLocks.h"

#include <boost/optional.hpp>

#include "CFGMutation.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "IRInstruction.h"
#include "ReachingDefinitions.h"

namespace copy_propagation_impl {
namespace locks {
namespace {

using namespace cfg;

struct MonitorData {
  IRInstruction* insn{nullptr};
  IRInstruction* source{nullptr};
  IRInstruction* immediate_in{nullptr};
};

struct RDefs {
  std::unordered_map<IRInstruction*, MonitorData> data;
  std::unordered_map<IRInstruction*, size_t> ordering;
};

boost::optional<RDefs> compute_rdefs(ControlFlowGraph& cfg) {
  // Do not use MoveAware, we want to track the moves.
  std::unique_ptr<reaching_defs::FixpointIterator> rdefs;
  auto get_defs = [&](Block* b, const IRInstruction* i) {
    if (!rdefs) {
      rdefs = std::make_unique<reaching_defs::FixpointIterator>(cfg);
      rdefs->run(reaching_defs::Environment());
    }
    auto defs_in = rdefs->get_entry_state_at(b);
    for (const auto& it : ir_list::InstructionIterable{b}) {
      if (it.insn == i) {
        break;
      }
      rdefs->analyze_instruction(it.insn, &defs_in);
    }
    return defs_in;
  };
  auto get_singleton = [](auto& defs, reg_t reg) -> IRInstruction* {
    const auto& defs0 = defs.get(reg);
    if (defs0.is_top() || defs0.is_bottom()) {
      return nullptr;
    }
    if (defs0.elements().size() != 1) {
      return nullptr;
    }
    return *defs0.elements().begin();
  };

  std::unordered_map<const IRInstruction*, Block*> block_map;
  auto get_rdef = [&](IRInstruction* insn, reg_t reg) -> IRInstruction* {
    auto it = block_map.find(insn);
    redex_assert(it != block_map.end());
    auto defs = get_defs(it->second, insn);
    return get_singleton(defs, reg);
  };

  std::vector<IRInstruction*> monitor_insns;
  for (auto* b : cfg.blocks()) {
    for (auto& mie : *b) {
      if (mie.type != MFLOW_OPCODE) {
        continue;
      }
      block_map.emplace(mie.insn, b);
      if (opcode::is_a_monitor(mie.insn->opcode())) {
        monitor_insns.push_back(mie.insn);
      }
    }
  }

  RDefs ret;
  size_t idx{0};
  for (auto* monitor_insn : monitor_insns) {
    auto find_root_def = [&](IRInstruction* cur) -> IRInstruction* {
      for (;;) {
        IRInstruction* next;
        switch (cur->opcode()) {
        case OPCODE_MONITOR_ENTER:
        case OPCODE_MONITOR_EXIT:
          next = get_rdef(cur, cur->src(0));
          break;

        case OPCODE_MOVE_OBJECT:
          next = get_rdef(cur, cur->src(0));
          break;

        case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT: {
          // This is complicated. If it is the move-result-pseudo
          // of a check-cast, continue. Otherwise use it.
          auto it = cfg.find_insn(cur);
          redex_assert(!it.is_end());
          auto prim_it = cfg.primary_instruction_of_move_result(it);
          redex_assert(!prim_it.is_end());

          if (prim_it->insn->opcode() != OPCODE_CHECK_CAST) {
            return cur;
          }
          next = prim_it->insn;
          break;
        }

        // Ignore check-cast, go further.
        case OPCODE_CHECK_CAST:
          next = get_rdef(cur, cur->src(0));
          break;

        // Includes move-result, which we take over the invoke etc.
        default:
          return cur;
        }
        if (next == nullptr) {
          return nullptr;
        }
        cur = next;
      }
    };

    auto immediate_rdef = get_rdef(monitor_insn, monitor_insn->src(0));
    if (immediate_rdef == nullptr) {
      return boost::none;
    }

    auto root_rdef = find_root_def(monitor_insn);
    if (root_rdef == nullptr) {
      return boost::none;
    }

    ret.data.emplace(monitor_insn,
                     MonitorData{monitor_insn, root_rdef, immediate_rdef});
    if (!ret.ordering.count(monitor_insn)) {
      ret.ordering[monitor_insn] = idx++;
    }
    if (!ret.ordering.count(root_rdef)) {
      ret.ordering[root_rdef] = idx++;
    }
  }

  return ret;
}

using MonitorGroups =
    std::vector<std::pair<IRInstruction*, std::vector<MonitorData*>>>;

MonitorGroups create_groups(RDefs& rdefs) {
  std::unordered_map<IRInstruction*, std::vector<MonitorData*>> tmp;
  for (const auto& p : rdefs.data) {
    MonitorData& data = rdefs.data[p.first];
    tmp[data.source].push_back(&data);
  }
  // Sort for determinism, use the instruction order from the data.
  MonitorGroups ret;
  ret.reserve(tmp.size());
  for (auto& p : tmp) {
    std::sort(p.second.begin(),
              p.second.end(),
              [&](const auto& lhs, const auto& rhs) {
                return rdefs.ordering.at(lhs->insn) <
                       rdefs.ordering.at(rhs->insn);
              });
    ret.emplace_back(p.first, std::move(p.second));
  }
  std::sort(ret.begin(), ret.end(), [&](const auto& lhs, const auto& rhs) {
    return rdefs.ordering.at(lhs.first) < rdefs.ordering.at(rhs.first);
  });
  return ret;
}

} // namespace

Result run(cfg::ControlFlowGraph& cfg) {
  Result res{};
  // 1) Run ReachingDefs.
  auto rdefs_opt = compute_rdefs(cfg);
  if (!rdefs_opt) {
    res.has_locks = true;
    res.non_singleton_rdefs = true;
    return res;
  }
  if (rdefs_opt->data.empty()) {
    return res;
  }
  res.has_locks = true;

  // 2) Create sets over the same source.
  auto groups = create_groups(*rdefs_opt);

  // 3) Process groups.
  CFGMutation mutation(cfg);
  for (const auto& p : groups) {
    auto& group = p.second;
    std::unordered_set<IRInstruction*> immediate_srcs;
    for (auto monitor_data : group) {
      immediate_srcs.insert(monitor_data->immediate_in);
    }
    if (immediate_srcs.size() == 1) {
      // This group is OK.
      continue;
    }
    auto source = (*group.begin())->source;

    // Make a copy.
    reg_t temp = cfg.allocate_temp();
    auto new_move = new IRInstruction(OPCODE_MOVE_OBJECT);
    new_move->set_src(0, source->dest());
    new_move->set_dest(temp);

    // Need to insert soon after instruction. However, if it is a load-param,
    // it must be at the end of all those.
    if (source->opcode() == IOPCODE_LOAD_PARAM_OBJECT) {
      auto it = cfg.find_insn(source);
      auto source_block = it.block();
      auto first_non_loading = source_block->get_first_non_param_loading_insn();
      if (first_non_loading != source_block->end()) {
        mutation.insert_before(
            source_block->to_cfg_instruction_iterator(first_non_loading),
            {new_move});
      } else {
        mutation.insert_after(source_block->to_cfg_instruction_iterator(
                                  source_block->get_last_insn()),
                              {new_move});
      }
    } else {
      mutation.insert_after(cfg.find_insn(source), {new_move});
    }

    // Fix up the elements.
    for (auto monitor_data : group) {
      monitor_data->insn->set_src(0, temp);
    }
    ++res.fixups;
  }
  mutation.flush();
  return res;
}

} // namespace locks
} // namespace copy_propagation_impl
