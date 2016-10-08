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

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "Transform.h"
#include "Walkers.h"

namespace {
/*
 * These instructions have observable side effects so must always be considered
 * live, regardless of whether their output is consumed by another instruction.
 */
static bool has_side_effects(DexOpcode opc) {
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
  case OPCODE_GOTO_16:
  case OPCODE_GOTO_32:
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
  case OPCODE_INVOKE_VIRTUAL_RANGE:
  case OPCODE_INVOKE_SUPER_RANGE:
  case OPCODE_INVOKE_DIRECT_RANGE:
  case OPCODE_INVOKE_STATIC_RANGE:
  case OPCODE_INVOKE_INTERFACE_RANGE:
  case FOPCODE_PACKED_SWITCH:
  case FOPCODE_SPARSE_SWITCH:
  case FOPCODE_FILLED_ARRAY:
    return true;
  default:
    return false;
  }
  not_reached();
}

/*
 * Pure methods have no observable side effects, so they can be removed if
 * their outputs are not used.
 *
 * TODO: Derive this list with static analysis rather than hard-coding it.
 */
std::unordered_set<DexMethod*> init_pure_methods() {
  std::unordered_set<DexMethod*> pure_methods;
  pure_methods.emplace(DexMethod::make_method(
      "Ljava/lang/Class;", "getSimpleName", "Ljava/lang/String;", {}));
  return pure_methods;
}

bool is_pure(DexMethod* method) {
  static std::unordered_set<DexMethod*> pure_methods = init_pure_methods();
  if (assumenosideeffects(method)) {
    return true;
  }
  return pure_methods.find(method) != pure_methods.end();
}

template <typename... T>
std::string show(const boost::dynamic_bitset<T...>& bits) {
  std::string ret;
  to_string(bits, ret);
  return ret;
}

////////////////////////////////////////////////////////////////////////////////

class LocalDce {
  size_t m_instructions_eliminated{0};
  size_t m_total_instructions{0};

  /*
   * Eliminate dead code using a standard backward dataflow analysis for
   * liveness.  The algorithm is as follows:
   *
   * - Maintain a bitvector for each block representing the liveness for each
   *   register.  Function call results are represented by bit #num_regs.
   *
   * - Walk the blocks in postorder. Compute each block's output state by
   *   OR-ing the liveness of its successors
   *
   * - Walk each block's instructions in reverse to determine its input state.
   *   An instruction's input registers are live if (a) it has side effects, or
   *   (b) its output registers are live.
   *
   * - If the liveness of any block changes during a pass, repeat it.  Since
   *   anything live in one pass is guaranteed to be live in the next, this is
   *   guaranteed to reach a fixed point and terminate.  Visiting blocks in
   *   postorder guarantees a minimum number of passes.
   *
   * - Catch blocks are handled slightly differently; since any instruction
   *   inside a `try` region can jump to a catch block, we assume that any
   *   registers that are live-in to a catch block must be kept live throughout
   *   the `try` region.  (This is actually conservative, since only
   *   potentially-excepting instructions can jump to a catch.)
   */
 public:
  void dce(DexMethod* method) {
    auto transform =
        MethodTransformer(method, true /* want_cfg */);
    auto& cfg = transform->cfg();
    auto blocks = PostOrderSort(cfg).get();
    auto regs = method->get_code()->get_registers_size();
    std::vector<boost::dynamic_bitset<>> liveness(
        cfg.size(), boost::dynamic_bitset<>(regs + 1));
    bool changed;
    std::vector<DexInstruction*> dead_instructions;

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
          if(s->id() == b->id()) {
            bliveness |= prev_liveness;
          }
          TRACE(DCE,
                5,
                "  S%lu: %s\n",
                s->id(),
                show(liveness[s->id()]).c_str());
          bliveness |= liveness[s->id()];
        }

        // Compute live-in for this block by walking its instruction list in
        // reverse and applying the liveness rules.
        for (auto it = b->rbegin(); it != b->rend(); ++it) {
          m_total_instructions++;
          if (it->type != MFLOW_OPCODE) {
            continue;
          }
          bool required = is_required(it->insn, bliveness);
          if (required) {
            update_liveness(it->insn, bliveness);
          } else {
            dead_instructions.push_back(it->insn);
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
    TRACE(DCE, 2, "%s\n", show(method).c_str());
    for (auto dead : dead_instructions) {
      TRACE(DCE, 2, "DEAD: %s\n", show(dead).c_str());
      transform->remove_opcode(dead);
      m_instructions_eliminated++;
    }

    remove_unreachable_blocks(method, *transform, cfg);

    TRACE(DCE, 5, "=== Post-DCE CFG ===\n");
    TRACE(DCE, 5, "%s", SHOW(cfg));
  }

 private:
  void remove_edge(Block* p, Block* s) {
    p->succs().erase(std::remove_if(p->succs().begin(),
                                    p->succs().end(),
                                    [&](Block* b) { return b == s; }),
                     p->succs().end());
    s->preds().erase(std::remove_if(s->preds().begin(),
                                    s->preds().end(),
                                    [&](Block* b) { return b == p; }),
                     s->preds().end());
  }

  bool can_delete(Block* b) {
    auto first = b->begin();
    if (first == b->end()) {
      return false;
    }
    for (auto last = b->rbegin(); last != b->rend(); ++last) {
      if (last->type == MFLOW_OPCODE &&
          last->insn->opcode() == FOPCODE_FILLED_ARRAY) {
        return false;
      }
    }
    // Skip if it contains nothing but debug info.
    for (; first != b->end(); ++first) {
      if (first->type != MFLOW_DEBUG || first->type != MFLOW_POSITION) {
        return true;
      }
    }
    return false;
  }

  void remove_block(MethodTransform* transform, Block* b) {
    if (!can_delete(b)) {
      return;
    }
    // Gather the ops to delete.
    std::unordered_set<DexInstruction*> delete_ops;
    std::unordered_set<MethodItemEntry*> delete_catches;
    for (auto& mei : *b) {
      if (mei.type == MFLOW_OPCODE) {
        delete_ops.insert(mei.insn);
      } else if (mei.type == MFLOW_TRY) {
        delete_catches.insert(mei.tentry->catch_start);
      } else if (mei.type == MFLOW_CATCH) {
        delete_catches.insert(&mei);
      }
    }
    // Remove branch targets.
    for (auto it = transform->begin(); it != transform->end(); ++it) {
      if (it->type == MFLOW_TARGET && delete_ops.count(it->target->src->insn)) {
        delete it->target;
        it->type = MFLOW_FALLTHROUGH;
      } else if (it->type == MFLOW_TRY &&
                 delete_catches.count(it->tentry->catch_start)) {
        delete it->tentry;
        it->type = MFLOW_FALLTHROUGH;
      } else if (it->type == MFLOW_CATCH &&
                 delete_catches.count(&*it)) {
        delete it->centry;
        it->type = MFLOW_FALLTHROUGH;
      }
    }
    // Remove the instructions.
    for (auto& op : delete_ops) {
      transform->remove_opcode(op);
    }
  }

  void remove_unreachable_blocks(DexMethod* method,
                                 MethodTransform* transform,
                                 std::vector<Block*>& blocks) {
    // Remove edges to catch blocks that no longer exist.
    std::vector<std::pair<Block*, Block*>> remove_edges;
    for (auto& b : blocks) {
      if (!is_catch(b)) {
        continue;
      }
      for (auto& p : b->preds()) {
        if (!ends_with_may_throw(p)) {
          // We removed whatever instruction could throw to this catch.
          remove_edges.emplace_back(p, b);
        }
      }
    }
    for (auto& e : remove_edges) {
      remove_edge(e.first, e.second);
    }
    // Iteratively remove blocks with no incoming edges.  Skip the first block
    // since it's the method entry point.
    std::vector<Block*> unreachables;
    for (size_t i = 1; i < blocks.size(); ++i) {
      if (blocks[i]->preds().size() == 0) {
        unreachables.push_back(blocks[i]);
      }
    }
    while (unreachables.size() > 0) {
      remove_edges.clear();
      auto& b = unreachables.back();
      auto succs = b->succs(); // copy
      for (auto& s : succs) {
        remove_edges.emplace_back(b, s);
      }
      for (auto& p : remove_edges) {
        remove_edge(p.first, p.second);
      }
      remove_block(transform, b);
      unreachables.pop_back();
      for (auto& s : succs) {
        if (s->preds().size() == 0) {
          unreachables.push_back(s);
        }
      }
    }
  }

  /*
   * An instruction is required (i.e., live) if it has side effects or if its
   * destination register is live.
   */
  bool is_required(DexInstruction* inst, const boost::dynamic_bitset<>& bliveness) {
    if (has_side_effects(inst->opcode())) {
      if (is_invoke(inst->opcode())) {
        auto invoke = static_cast<DexOpcodeMethod*>(inst);
        if (!is_pure(invoke->get_method())) {
          return true;
        }
        return bliveness.test(bliveness.size() - 1);
      }
      return true;
    } else if (inst->dests_size()) {
      return bliveness.test(inst->dest());
    } else if (is_filled_new_array(inst->opcode())) {
      // filled-new-array passes its dest via the return-value slot, but isn't
      // inherently live like the invoke-* instructions.
      return bliveness.test(bliveness.size() - 1);
    }
    return false;
  }

  /*
   * Update the liveness vector given that `inst` is live.
   */
  void update_liveness(const DexInstruction* inst,
                       boost::dynamic_bitset<>& bliveness) {
    // The destination register is killed, so it isn't live before this.
    if (inst->dests_size()) {
      bliveness.reset(inst->dest());
    }
    // The destination of an `invoke` is its return value, which is encoded as
    // the max position in the bitvector.
    if (is_invoke(inst->opcode()) || is_filled_new_array(inst->opcode())) {
      bliveness.reset(bliveness.size() - 1);
    }
    // Source registers are live.
    for (size_t i = 0; i < inst->srcs_size(); i++) {
      bliveness.set(inst->src((int)i));
    }
    // `invoke-range` instructions need special handling since their sources
    // are encoded as a range.
    if (inst->has_range()) {
      for (size_t i = 0; i < inst->range_size(); i++) {
        bliveness.set(inst->range_base() + i);
      }
    }
    // The source of a `move-result` is the return value of the prior call,
    // which is encoded as the max position in the bitvector.
    if (is_move_result(inst->opcode())) {
      bliveness.set(bliveness.size() - 1);
    }
  }

 public:
  void run(const Scope& scope) {
	  TRACE(DCE, 1, "Running LocalDCE pass\n");
    walk_methods(scope,
                 [&](DexMethod* m) {
                   if (!m->get_code()) {
                     return;
                   }
                   dce(m);
                 });
    TRACE(DCE, 1,
            "Dead instructions eliminated: %lu\n",
            m_instructions_eliminated);
    TRACE(DCE, 1,
            "Total instructions: %lu\n",
            m_total_instructions);
    TRACE(DCE, 1,
            "Percentage of instructions identified as dead code: %f%%\n",
            m_instructions_eliminated * 100 / double(m_total_instructions));
  }
};
}

void LocalDcePass::run(DexMethod* m) {
  LocalDce().dce(m);
}

void LocalDcePass::run_pass(DexStoresVector& stores, ConfigFiles& cfg, PassManager& mgr) {
  auto scope = build_class_scope(stores);
  LocalDce().run(scope);
}

static LocalDcePass s_pass;
