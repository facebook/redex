/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "LocalDce.h"

#include <iostream>
#include <array>
#include <unordered_set>
#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "DexUtil.h"
#include "Resolver.h"
#include "Transform.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_DEAD_INSTRUCTIONS = "num_dead_instructions";
constexpr const char* METRIC_UNREACHABLE_INSTRUCTIONS =
    "num_unreachable_instructions";

/*
 * These instructions have observable side effects so must always be considered
 * live, regardless of whether their output is consumed by another instruction.
 */
static bool has_side_effects(IROpcode opc) {
  switch (opc) {
  case OPCODE_RETURN_VOID:
  case OPCODE_RETURN:
  case OPCODE_RETURN_WIDE:
  case OPCODE_RETURN_OBJECT:
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_CHECK_CAST:
  case OPCODE_FILL_ARRAY_DATA:
  case OPCODE_THROW:
  case OPCODE_GOTO:
  case OPCODE_PACKED_SWITCH:
  case OPCODE_SPARSE_SWITCH:
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
  case OPCODE_APUT:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_OBJECT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT:
  case OPCODE_IPUT:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT:
  case OPCODE_SPUT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT:
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE:
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE:
    return true;
  default:
    return false;
  }
  not_reached();
}

template <typename... T>
std::string show(const boost::dynamic_bitset<T...>& bits) {
  std::string ret;
  to_string(bits, ret);
  return ret;
}

void remove_empty_try_regions(IRCode* code) {
  // comb the method looking for superfluous try sections that do not enclose
  // throwing opcodes; remove them. note that try sections should never be
  // nested, otherwise this won't produce the right result.
  bool encloses_throw{false};
  MethodItemEntry* try_start{nullptr};
  for (auto& mie : *code) {
    if (mie.type == MFLOW_TRY) {
      auto tentry = mie.tentry;
      if (tentry->type == TRY_START) {
        encloses_throw = false;
        try_start = &mie;
      } else if (!encloses_throw /* && tentry->type == TRY_END */) {
        try_start->type = MFLOW_FALLTHROUGH;
        try_start = nullptr;
        mie.type = MFLOW_FALLTHROUGH;
      }
    } else if (mie.type == MFLOW_OPCODE) {
      auto op = mie.insn->opcode();
      encloses_throw =
          encloses_throw || opcode::may_throw(op) || op == OPCODE_THROW;
    }
  }
}

/*
 * Update the liveness vector given that `inst` is live.
 */
void update_liveness(const IRInstruction* inst,
                     boost::dynamic_bitset<>& bliveness) {
  // The destination register is killed, so it isn't live before this.
  if (inst->dests_size()) {
    bliveness.reset(inst->dest());
  }
  auto op = inst->opcode();
  // The destination of an `invoke` is its return value, which is encoded as
  // the max position in the bitvector.
  if (is_invoke(op) || is_filled_new_array(op) ||
      inst->has_move_result_pseudo()) {
    bliveness.reset(bliveness.size() - 1);
  }
  // Source registers are live.
  for (size_t i = 0; i < inst->srcs_size(); i++) {
    bliveness.set(inst->src(i));
  }
  // The source of a `move-result` is the return value of the prior call,
  // which is encoded as the max position in the bitvector.
  if (is_move_result(op) || opcode::is_move_result_pseudo(op)) {
    bliveness.set(bliveness.size() - 1);
  }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

void LocalDce::dce(DexMethod* method) {
  auto code = method->get_code();
  code->build_cfg();
  auto& cfg = code->cfg();
  auto blocks = cfg::postorder_sort(cfg.blocks());
  auto regs = method->get_code()->get_registers_size();
  std::vector<boost::dynamic_bitset<>> liveness(
      cfg.blocks().size(), boost::dynamic_bitset<>(regs + 1));
  bool changed;
  std::vector<IRList::iterator> dead_instructions;

  TRACE(DCE, 5, "%s\n", SHOW(method));
  TRACE(DCE, 5, "%s", SHOW(cfg));

  // Iterate liveness analysis to a fixed point.
  do {
    changed = false;
    dead_instructions.clear();
    for (auto& b : blocks) {
      auto prev_liveness = liveness.at(b->id());
      auto& bliveness = liveness.at(b->id());
      bliveness.reset();
      TRACE(DCE, 5, "B%lu: %s\n", b->id(), show(bliveness).c_str());

      // Compute live-out for this block from its successors.
      for (auto& s : b->succs()) {
        if (s->target()->id() == b->id()) {
          bliveness |= prev_liveness;
        }
        TRACE(DCE,
              5,
              "  S%lu: %s\n",
              s->target()->id(),
              SHOW(liveness[s->target()->id()]));
        bliveness |= liveness[s->target()->id()];
      }

      // Compute live-in for this block by walking its instruction list in
      // reverse and applying the liveness rules.
      for (auto it = b->rbegin(); it != b->rend(); ++it) {
        if (it->type != MFLOW_OPCODE) {
          continue;
        }
        bool required = is_required(it->insn, bliveness);
        if (required) {
          update_liveness(it->insn, bliveness);
        } else {
          // move-result-pseudo instructions will be automatically removed
          // when their primary instruction is deleted.
          if (!opcode::is_move_result_pseudo(it->insn->opcode())) {
            dead_instructions.push_back(std::prev(it.base()));
          }
        }
        TRACE(CFG,
              5,
              "%s\n%s\n",
              show(it->insn).c_str(),
              show(bliveness).c_str());
      }
      if (bliveness != prev_liveness) {
        changed = true;
      }
    }
  } while (changed);

  // Remove dead instructions.
  TRACE(DCE, 2, "%s\n", SHOW(method));
  std::unordered_set<IRInstruction*> seen;
  for (auto dead : dead_instructions) {
    if (seen.count(dead->insn)) {
      continue;
    }
    TRACE(DCE, 2, "DEAD: %s\n", SHOW(dead->insn));
    code->remove_opcode(dead);
    seen.emplace(dead->insn);
  }
  m_stats.dead_instruction_count += dead_instructions.size();

  // if we deleted an instruction that may throw, we'll need to remove any
  // EDGE_THROW edges in the CFG... ideally we would just prune that edge,
  // but we can do a conservative and inefficient hack for now and just
  // rebuild the entire graph
  if (dead_instructions.size() > 0) {
    code->build_cfg();
  }

  m_stats.unreachable_instruction_count +=
      transform::remove_unreachable_blocks(code);
  remove_empty_try_regions(code);

  TRACE(DCE, 5, "=== Post-DCE CFG ===\n");
  TRACE(DCE, 5, "%s", SHOW(code->cfg()));
}

/*
 * An instruction is required (i.e., live) if it has side effects or if its
 * destination register is live.
 */
bool LocalDce::is_required(IRInstruction* inst,
                           const boost::dynamic_bitset<>& bliveness) {
  if (has_side_effects(inst->opcode())) {
    if (is_invoke(inst->opcode())) {
      const auto meth =
          resolve_method(inst->get_method(), opcode_to_search(inst));
      if (!is_pure(inst->get_method(), meth)) {
        return true;
      }
      return bliveness.test(bliveness.size() - 1);
    }
    return true;
  } else if (inst->dests_size()) {
    return bliveness.test(inst->dest());
  } else if (is_filled_new_array(inst->opcode()) ||
             inst->has_move_result_pseudo()) {
    // These instructions pass their dests via the return-value slot, but
    // aren't inherently live like the invoke-* instructions.
    return bliveness.test(bliveness.size() - 1);
  }
  return false;
}

bool LocalDce::is_pure(DexMethodRef* ref, DexMethod* meth) {
  if (meth != nullptr && assumenosideeffects(meth)) {
    return true;
  }
  return m_pure_methods.find(ref) != m_pure_methods.end();
}

void LocalDcePass::run(DexMethod* m) {
  LocalDce(find_pure_methods()).dce(m);
}

void LocalDcePass::run_pass(DexStoresVector& stores,
                            ConfigFiles& cfg,
                            PassManager& mgr) {
  if (mgr.no_proguard_rules()) {
    TRACE(DCE, 1,
        "LocalDcePass not run because no ProGuard configuration was provided.");
    return;
  }
  const auto& pure_methods = find_pure_methods();
  auto scope = build_class_scope(stores);
  auto stats = walk::parallel::reduce_methods<std::nullptr_t, LocalDce::Stats>(
      scope,
      [&](std::nullptr_t, DexMethod* m) {
        auto* code = m->get_code();
        if (code == nullptr) {
          return LocalDce::Stats();
        }
        LocalDce ldce(pure_methods);
        ldce.dce(m);
        return ldce.get_stats();
      },
      [](LocalDce::Stats a, LocalDce::Stats b) {
        a.dead_instruction_count += b.dead_instruction_count;
        a.unreachable_instruction_count += b.unreachable_instruction_count;
        return a;
      },
      [](int) { return nullptr; });
  mgr.incr_metric(METRIC_DEAD_INSTRUCTIONS, stats.dead_instruction_count);
  mgr.incr_metric(METRIC_UNREACHABLE_INSTRUCTIONS,
                  stats.unreachable_instruction_count);
}

std::unordered_set<DexMethodRef*> LocalDcePass::find_pure_methods() {
  /*
   * Pure methods have no observable side effects, so they can be removed
   * if their outputs are not used.
   *
   * TODO: Derive this list with static analysis rather than hard-coding
   * it.
   */
  std::unordered_set<DexMethodRef*> pure_methods;
  pure_methods.emplace(DexMethod::make_method(
      "Ljava/lang/Class;", "getSimpleName", "Ljava/lang/String;", {}));
  return pure_methods;
}

static LocalDcePass s_pass;
