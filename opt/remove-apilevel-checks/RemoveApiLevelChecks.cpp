/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveApiLevelChecks.h"

#include <boost/optional.hpp>

#include "ControlFlow.h"
#include "PassManager.h"
#include "ReachingDefinitions.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Walkers.h"

namespace {

using namespace cfg;

std::unordered_set<const IRInstruction*> find_sdk_int_sgets(
    const ControlFlowGraph& cfg, const DexFieldRef* sdk_int_field) {
  std::unordered_set<const IRInstruction*> ret;
  for (const auto& it : ConstInstructionIterable(cfg)) {
    if (it.insn->opcode() == OPCODE_SGET) {
      auto f = it.insn->get_field();
      if (f == sdk_int_field) {
        ret.insert(it.insn);
      }
    }
  }
  return ret;
}

IROpcode get_symmetric_cond(IROpcode op) {
  switch (op) {
  case OPCODE_IF_EQ:
    return OPCODE_IF_EQ;
  case OPCODE_IF_NE:
    return OPCODE_IF_NE;
  case OPCODE_IF_LT:
    return OPCODE_IF_GT;
  case OPCODE_IF_GE:
    return OPCODE_IF_LE;
  case OPCODE_IF_GT:
    return OPCODE_IF_LT;
  case OPCODE_IF_LE:
    return OPCODE_IF_GE;
  default:
    not_reached_log("Invalid conditional opcode %s", SHOW(op));
  }
}

boost::optional<bool> analyze1(
    IROpcode op,
    IRInstruction* i0,
    const std::unordered_set<const IRInstruction*>& sgets,
    int32_t min_sdk) {
  if (sgets.count(i0) == 0) {
    return boost::none;
  }

  // Is "x (op) cmp_val = result" for all "x >= min_sdk" ?
  switch (op) {
  case OPCODE_IF_EQZ:
    if (min_sdk > 0) {
      return false;
    }
    break;
  case OPCODE_IF_NEZ:
    if (min_sdk > 0) {
      return true;
    }
    break;

  case OPCODE_IF_LEZ:
    if (min_sdk > 0) {
      return false;
    }
    break;
  case OPCODE_IF_LTZ:
    if (min_sdk >= 0) {
      return false;
    }
    break;

  case OPCODE_IF_GEZ:
    if (min_sdk >= 0) {
      return true;
    }
    break;
  case OPCODE_IF_GTZ:
    if (min_sdk > 0) {
      return true;
    }
    break;

  default:
    break;
  }

  return boost::none;
};

boost::optional<bool> analyze2(
    IROpcode op,
    IRInstruction* i0,
    IRInstruction* i1,
    const std::unordered_set<const IRInstruction*>& sgets,
    int32_t min_sdk) {
  if (sgets.count(i0) == 0 && sgets.count(i1) == 0) {
    return boost::none;
  }

  // Normalize: want "min_sdk (op) value."
  IRInstruction* cmp = i1;
  if (sgets.count(i1) > 0) {
    // This is not inversion!
    cmp = i0;
    op = get_symmetric_cond(op);
  }

  if (cmp->opcode() != OPCODE_CONST) {
    return boost::none;
  }
  int64_t cmp_val = cmp->get_literal();

  // Is "x (op) cmp_val = result" for all "x >= min_sdk" ?
  switch (op) {
  case OPCODE_IF_LT:
    if (min_sdk >= cmp_val) {
      return false;
    }
    break;
  case OPCODE_IF_LE:
    if (min_sdk > cmp_val) {
      return false;
    }
    break;

  case OPCODE_IF_GT:
    if (min_sdk > cmp_val) {
      return true;
    }
    break;
  case OPCODE_IF_GE:
    if (min_sdk >= cmp_val) {
      return true;
    }
    break;

  default:
    break;
  }

  return boost::none;
};

size_t analyze_and_rewrite(
    ControlFlowGraph& cfg,
    const std::unordered_set<const IRInstruction*>& sgets,
    int32_t min_sdk) {
  std::unique_ptr<reaching_defs::MoveAwareFixpointIterator> rdefs;
  auto get_defs = [&](Block* b, IRInstruction* i) {
    if (!rdefs) {
      rdefs.reset(new reaching_defs::MoveAwareFixpointIterator(cfg));
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
    auto defs0 = defs.get(reg);
    if (defs0.is_top() || defs0.is_bottom()) {
      return nullptr;
    }
    if (defs0.elements().size() != 1) {
      return nullptr;
    }
    return *defs0.elements().begin();
  };

  size_t removed = 0;

  for (auto* b : cfg.blocks()) {
    auto it = b->get_last_insn();
    if (it == b->end()) {
      continue;
    }
    auto* insn = it->insn;
    IROpcode op = insn->opcode();
    if (!opcode::is_a_conditional_branch(op)) {
      continue;
    }

    auto defs = get_defs(b, insn);

    IRInstruction* i0 = get_singleton(defs, insn->src(0));
    if (i0 == nullptr) {
      continue;
    }

    boost::optional<bool> result = boost::none;
    if (insn->srcs_size() == 1) {
      result = analyze1(op, i0, sgets, min_sdk);
    } else if (insn->srcs_size() == 2) {
      IRInstruction* i1 = get_singleton(defs, insn->src(1));
      if (i1 == nullptr) {
        continue;
      }
      result = analyze2(op, i0, i1, sgets, min_sdk);
    }
    if (!result) {
      continue;
    }

    if (*result) {
      // Change the GOTO edge to point to the BRANCH edge's target.
      auto branch_edge = cfg.get_succ_edge_of_type(b, EDGE_BRANCH);
      redex_assert(branch_edge);
      auto goto_edge = cfg.get_succ_edge_of_type(b, EDGE_GOTO);
      redex_assert(goto_edge);
      cfg.set_edge_target(goto_edge, branch_edge->target());
    }
    cfg.remove_insn(b->to_cfg_instruction_iterator(it));

    ++removed;
  }
  return removed;
}

} // namespace

const DexFieldRef* RemoveApiLevelChecksPass::get_sdk_int_field() {
  return DexField::get_field(DexType::make_type("Landroid/os/Build$VERSION;"),
                             DexString::make_string("SDK_INT"),
                             DexType::get_type("I"));
}

RemoveApiLevelChecksPass::ApiLevelStats RemoveApiLevelChecksPass::run(
    IRCode* code, int32_t min_sdk, const DexFieldRef* sdk_int_field) {
  if (!code) {
    return ApiLevelStats{};
  }

  cfg::ScopedCFG cfg(code);

  auto sdk_int_sgets = find_sdk_int_sgets(*cfg, sdk_int_field);
  if (sdk_int_sgets.empty()) {
    return ApiLevelStats();
  }

  ApiLevelStats ret;
  ret.num_field_gets = sdk_int_sgets.size();
  ret.num_methods = 1;
  ret.num_removed = analyze_and_rewrite(*cfg, sdk_int_sgets, min_sdk);
  return ret;
}

void RemoveApiLevelChecksPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles&,
                                        PassManager& mgr) {
  auto scope = build_class_scope(stores);
  auto min_sdk = mgr.get_redex_options().min_sdk;
  const auto* sdk_int_field = get_sdk_int_field();
  redex_assert(sdk_int_field != nullptr);

  ApiLevelStats stats = walk::parallel::methods<ApiLevelStats>(
      scope, [min_sdk, sdk_int_field](DexMethod* method) -> ApiLevelStats {
        auto code = method->get_code();
        return run(code, min_sdk, sdk_int_field);
      });
  mgr.set_metric("min_sdk", min_sdk);
  mgr.incr_metric("num_field_gets", stats.num_field_gets);
  mgr.incr_metric("num_methods", stats.num_methods);
  mgr.incr_metric("num_optimized", stats.num_removed);
}

static RemoveApiLevelChecksPass s_pass;
