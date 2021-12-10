/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LocalDce.h"

#include <array>
#include <iostream>
#include <unordered_set>
#include <vector>

#include "CFGMutation.h"
#include "ControlFlow.h"
#include "DexClass.h"
#include "DexUtil.h"
#include "GraphUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "MethodOverrideGraph.h"
#include "NullPointerExceptionUtil.h"
#include "Purity.h"
#include "ReachingDefinitions.h"
#include "Resolver.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include "Transform.h"
#include "TypeSystem.h"
#include "Walkers.h"

namespace {

template <typename... T>
std::string show(const boost::dynamic_bitset<T...>& bits) {
  std::string ret;
  to_string(bits, ret);
  return ret;
}

/*
 * Update the liveness vector given that `inst` is live.
 */
void update_liveness(const IRInstruction* inst,
                     boost::dynamic_bitset<>& bliveness) {
  // The destination register is killed, so it isn't live before this.
  if (inst->has_dest()) {
    bliveness.reset(inst->dest());
  }
  auto op = inst->opcode();
  // The destination of an `invoke` is its return value, which is encoded as
  // the max position in the bitvector.
  if (opcode::is_an_invoke(op) || opcode::is_filled_new_array(op) ||
      inst->has_move_result_pseudo()) {
    bliveness.reset(bliveness.size() - 1);
  }
  // Source registers are live.
  for (size_t i = 0; i < inst->srcs_size(); i++) {
    bliveness.set(inst->src(i));
  }
  // The source of a `move-result` is the return value of the prior call,
  // which is encoded as the max position in the bitvector.
  if (opcode::is_move_result_any(op)) {
    bliveness.set(bliveness.size() - 1);
  }
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

std::vector<std::pair<cfg::Block*, IRList::iterator>>
LocalDce::get_dead_instructions(const cfg::ControlFlowGraph& cfg,
                                const std::vector<cfg::Block*>& blocks,
                                bool* any_init_class_insns) {
  auto regs = cfg.get_registers_size();
  std::unordered_map<cfg::BlockId, boost::dynamic_bitset<>> liveness;
  for (cfg::Block* b : cfg.blocks()) {
    liveness.emplace(b->id(), boost::dynamic_bitset<>(regs + 1));
  }
  bool changed;
  std::vector<std::pair<cfg::Block*, IRList::iterator>> dead_instructions;

  // Iterate liveness analysis to a fixed point.
  do {
    changed = false;
    dead_instructions.clear();
    for (auto& b : blocks) {
      auto prev_liveness = liveness.at(b->id());
      auto& bliveness = liveness.at(b->id());
      bliveness.reset();
      TRACE(DCE, 5, "B%lu: %s", b->id(), show(bliveness).c_str());

      // Compute live-out for this block from its successors.
      for (auto& s : b->succs()) {
        if (s->target()->id() == b->id()) {
          bliveness |= prev_liveness;
        }
        TRACE(DCE,
              5,
              "  S%lu: %s",
              s->target()->id(),
              SHOW(liveness.at(s->target()->id())));
        bliveness |= liveness.at(s->target()->id());
      }

      // Compute live-in for this block by walking its instruction list in
      // reverse and applying the liveness rules.
      for (auto it = b->rbegin(); it != b->rend(); ++it) {
        if (it->type != MFLOW_OPCODE) {
          continue;
        }
        bool required = is_required(cfg, b, it->insn, bliveness);
        if (required) {
          update_liveness(it->insn, bliveness);
          if (it->insn->opcode() == IOPCODE_INIT_CLASS) {
            *any_init_class_insns = true;
          }
        } else {
          // move-result-pseudo instructions will be automatically removed
          // when their primary instruction is deleted.
          if (!opcode::is_a_move_result_pseudo(it->insn->opcode())) {
            auto forward_it = std::prev(it.base());
            dead_instructions.emplace_back(b, forward_it);
          }
        }
        TRACE(CFG, 5, "%s\n%s", show(it->insn).c_str(),
              show(bliveness).c_str());
      }
      if (bliveness != prev_liveness) {
        changed = true;
      }
    }
  } while (changed);
  return dead_instructions;
}

void LocalDce::dce(cfg::ControlFlowGraph& cfg,
                   bool normalize_new_instances,
                   DexType* declaring_type) {
  if (normalize_new_instances) {
    this->normalize_new_instances(cfg);
  }
  TRACE(DCE, 5, "%s", SHOW(cfg));
  const auto& blocks = graph::postorder_sort<cfg::GraphInterface>(cfg);
  bool any_init_class_insns = false;
  std::vector<std::pair<cfg::Block*, IRList::iterator>> dead_instructions =
      get_dead_instructions(cfg, blocks, &any_init_class_insns);

  // Remove dead instructions.
  std::unordered_set<IRInstruction*> seen;
  cfg::CFGMutation mutation(cfg);
  std::unique_ptr<npe::NullPointerExceptionCreator> npe_creator;
  size_t npe_instructions = 0;
  size_t init_class_instructions_added = 0;
  for (const auto& pair : dead_instructions) {
    cfg::Block* b = pair.first;
    const IRList::iterator& it = pair.second;
    auto insn = it->insn;
    if (!seen.emplace(insn).second) {
      continue;
    }
    auto cfg_it = b->to_cfg_instruction_iterator(it);
    DexMethod* method;
    if (m_may_allocate_registers && m_method_override_graph &&
        (insn->opcode() == OPCODE_INVOKE_VIRTUAL ||
         insn->opcode() == OPCODE_INVOKE_INTERFACE) &&
        (method = resolve_method(insn->get_method(), opcode_to_search(insn))) !=
            nullptr &&
        !has_implementor(m_method_override_graph, method)) {
      TRACE(DCE, 2, "DEAD NPE: %s", SHOW(insn));
      if (!npe_creator) {
        npe_creator = std::make_unique<npe::NullPointerExceptionCreator>(&cfg);
      }
      cfg.replace_insns(cfg_it, npe_creator->get_insns(insn));
      npe_instructions++;
    } else {
      TRACE(DCE, 2, "DEAD: %s", SHOW(insn));
      auto init_class_insn =
          m_init_classes_with_side_effects
              ? m_init_classes_with_side_effects->create_init_class_insn(
                    get_init_class_type_demand(insn))
              : nullptr;
      if (init_class_insn) {
        init_class_instructions_added++;
        mutation.replace(cfg_it, {init_class_insn});
        any_init_class_insns = true;
      } else {
        mutation.remove(cfg_it);
      }
    }
  }
  mutation.flush();

  if (any_init_class_insns && m_init_classes_with_side_effects &&
      declaring_type) {
    prune_init_classes(cfg, declaring_type);
  }

  auto unreachable_insn_count = cfg.remove_unreachable_blocks().first;
  cfg.recompute_registers_size();

  m_stats.npe_instruction_count += npe_instructions;
  m_stats.init_class_instructions_added += init_class_instructions_added;
  m_stats.dead_instruction_count += dead_instructions.size();
  m_stats.unreachable_instruction_count += unreachable_insn_count;

  TRACE(DCE, 5, "=== Post-DCE CFG ===");
  TRACE(DCE, 5, "%s", SHOW(cfg));
}

void LocalDce::dce(IRCode* code,
                   bool normalize_new_instances,
                   DexType* declaring_type) {
  cfg::ScopedCFG cfg(code);
  dce(*cfg, normalize_new_instances, declaring_type);
}

/*
 * An instruction is required (i.e., live) if it has side effects or if its
 * destination register is live.
 */
bool LocalDce::is_required(const cfg::ControlFlowGraph& cfg,
                           cfg::Block* b,
                           IRInstruction* inst,
                           const boost::dynamic_bitset<>& bliveness) {
  if (opcode::has_side_effects(inst->opcode())) {
    if (opcode::is_an_invoke(inst->opcode())) {
      const auto meth =
          resolve_method(inst->get_method(), opcode_to_search(inst));
      if (meth == nullptr) {
        return true;
      }
      if (!assumenosideeffects(inst->get_method(), meth)) {
        return true;
      }
      if (!m_init_classes_with_side_effects &&
          inst->opcode() == OPCODE_INVOKE_STATIC) {
        if (!m_ignore_pure_method_init_classes ||
            !m_pure_methods.count(inst->get_method())) {
          return true;
        }
      }
      return bliveness.test(bliveness.size() - 1);
    } else if (opcode::is_a_conditional_branch(inst->opcode())) {
      cfg::Edge* goto_edge = cfg.get_succ_edge_of_type(b, cfg::EDGE_GOTO);
      cfg::Edge* branch_edge = cfg.get_succ_edge_of_type(b, cfg::EDGE_BRANCH);
      always_assert(goto_edge != nullptr);
      always_assert(branch_edge != nullptr);
      return goto_edge->target() != branch_edge->target();
    } else if (opcode::is_switch(inst->opcode())) {
      cfg::Edge* goto_edge = cfg.get_succ_edge_of_type(b, cfg::EDGE_GOTO);
      always_assert(goto_edge != nullptr);
      auto branch_edges = cfg.get_succ_edges_of_type(b, cfg::EDGE_BRANCH);
      for (cfg::Edge* branch_edge : branch_edges) {
        if (goto_edge->target() != branch_edge->target()) {
          return true;
        }
      }
      return false;
    }
    return true;
  } else if (inst->has_dest()) {
    return bliveness.test(inst->dest());
  } else if (opcode::is_filled_new_array(inst->opcode()) ||
             inst->has_move_result_pseudo()) {
    if (opcode::is_an_sget(inst->opcode())) {
      auto field = resolve_field(inst->get_field(), FieldSearch::Static);
      if (field && field->rstate.init_class()) {
        return true;
      }
    }
    if (!m_init_classes_with_side_effects &&
        (inst->opcode() == OPCODE_NEW_INSTANCE ||
         opcode::is_an_sfield_op(inst->opcode()))) {
      return true;
    }
    // These instructions pass their dests via the return-value slot, but
    // aren't inherently live like the invoke-* instructions.
    return bliveness.test(bliveness.size() - 1);
  }
  return false;
}

bool LocalDce::assumenosideeffects(DexMethodRef* ref, DexMethod* meth) {
  if (::assumenosideeffects(meth)) {
    return true;
  }
  return m_pure_methods.find(ref) != m_pure_methods.end();
}

void LocalDce::normalize_new_instances(cfg::ControlFlowGraph& cfg) {
  // TODO: This normalization optimization doesn't really belong to local-dce,
  // but it combines nicely as local-dce will clean-up redundant new-instance
  // instructions and moves afterwards.

  // Let's not do the transformation if there's a chance that it could leave
  // behind dangling new-instance instructions that LocalDce couldn't remove.
  if (!m_init_classes_with_side_effects) {
    return;
  }

  cfg::CFGMutation mutation(cfg);
  reaching_defs::MoveAwareFixpointIterator fp_iter(cfg);
  fp_iter.run({});
  for (cfg::Block* block : cfg.blocks()) {
    auto env = fp_iter.get_entry_state_at(block);
    if (env.is_bottom()) {
      continue;
    }
    auto ii = InstructionIterable(block);
    for (auto it = ii.begin(), end = ii.end(), last_insn = end; it != end;
         last_insn = it, fp_iter.analyze_instruction(it->insn, &env), it++) {
      IRInstruction* insn = it->insn;
      if (insn->opcode() != OPCODE_INVOKE_DIRECT ||
          !method::is_init(insn->get_method())) {
        continue;
      }
      auto type = insn->get_method()->get_class();
      auto reg = insn->src(0);
      const auto& defs = env.get(reg);
      always_assert(!defs.is_top());
      always_assert(!defs.is_bottom());
      IRInstruction* old_new_instance_insn{nullptr};
      for (auto def : defs.elements()) {
        if (def->opcode() == OPCODE_NEW_INSTANCE) {
          always_assert(old_new_instance_insn == nullptr);
          old_new_instance_insn = def;
          always_assert(def->get_type() == type);
        }
      }
      if (old_new_instance_insn == nullptr) {
        // base constructor invocation
        continue;
      }
      if (last_insn != end &&
          last_insn->insn->opcode() == IOPCODE_MOVE_RESULT_PSEUDO_OBJECT &&
          last_insn->insn->dest() == reg) {
        auto primary_insn = cfg.primary_instruction_of_move_result(
            block->to_cfg_instruction_iterator(last_insn));
        if (primary_insn->insn->opcode() == OPCODE_NEW_INSTANCE) {
          always_assert(primary_insn->insn->get_type() == type);
          // already normalized
          continue;
        }
      }

      // Let's detect aliases which might have been created via move-object
      // instructions.
      bool aliased{false};
      for (const auto& pair : env.bindings()) {
        auto other_defs = pair.second;
        always_assert(!other_defs.is_top());
        always_assert(!other_defs.is_bottom());
        if (other_defs.contains(old_new_instance_insn) && pair.first != reg) {
          aliased = true;
          break;
        }
      }
      if (aliased) {
        // Don't touch this; maybe this will go away after another round of
        // copy-propagation / local-dce.
        m_stats.aliased_new_instances++;
        continue;
      }

      // Scan for the move-result-pseudo and a source block afterwards.
      std::unique_ptr<SourceBlock> sb_move;
      {
        auto original_move_cfg_it =
            cfg.move_result_of(cfg.find_insn(old_new_instance_insn, block));
        redex_assert(!original_move_cfg_it.is_end());
        auto* move_block = original_move_cfg_it.block();
        auto original_move_it = original_move_cfg_it.unwrap();
        ++original_move_it;
        while (original_move_it != move_block->end()) {
          if (original_move_it->type == MFLOW_OPCODE) {
            break;
          }
          if (original_move_it->type == MFLOW_SOURCE_BLOCK) {
            sb_move = std::move(original_move_it->src_block);
            block->remove_mie(original_move_it);
            break;
          }
          ++original_move_it;
        }
      }

      // We don't bother removing the old new-instance instruction (or other
      // intermediate move-object instructions) here, as LocalDce will do that
      // as part of its normal operation.
      auto new_instance_insn = new IRInstruction(OPCODE_NEW_INSTANCE);
      new_instance_insn->set_type(type);
      auto move_result_pseudo_object_insn =
          new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT);
      move_result_pseudo_object_insn->set_dest(reg);
      if (sb_move == nullptr) {
        mutation.insert_before(
            block->to_cfg_instruction_iterator(it),
            {new_instance_insn, move_result_pseudo_object_insn});
      } else {
        std::vector<cfg::ControlFlowGraph::InsertVariant> tmp;
        tmp.emplace_back(new_instance_insn);
        tmp.emplace_back(move_result_pseudo_object_insn);
        tmp.emplace_back(std::move(sb_move));
        mutation.insert_before_var(block->to_cfg_instruction_iterator(it),
                                   std::move(tmp));
      }
      m_stats.normalized_new_instances++;
    }
  }
  mutation.flush();
}

void LocalDce::prune_init_classes(cfg::ControlFlowGraph& cfg,
                                  DexType* declaring_type) {
  init_classes::InitClassPruner init_class_pruner(
      *m_init_classes_with_side_effects, declaring_type, cfg);
  init_class_pruner.apply();
  m_stats.init_classes = init_class_pruner.get_stats();
}
