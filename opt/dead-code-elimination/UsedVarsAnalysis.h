/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexUtil.h"
#include "LocalPointersAnalysis.h"
#include "PatriciaTreeSetAbstractDomain.h"
#include "ReducedProductAbstractDomain.h"
#include "Resolver.h"
#include "SideEffectSummary.h"

using reg_t = uint32_t;

constexpr reg_t RESULT_REGISTER = std::numeric_limits<reg_t>::max();

using UsedRegisters = sparta::PatriciaTreeSetAbstractDomain<reg_t>;

using UsedPointers =
    sparta::PatriciaTreeSetAbstractDomain<const IRInstruction*>;

class UsedVarsSet final
    : public sparta::ReducedProductAbstractDomain<UsedVarsSet,
                                                  UsedRegisters,
                                                  UsedPointers> {
 public:
  using ReducedProductAbstractDomain::ReducedProductAbstractDomain;

  static void reduce_product(const std::tuple<UsedRegisters, UsedPointers>&) {}

  const UsedRegisters& get_used_registers() const { return get<0>(); }

  const UsedPointers& get_used_pointers() const { return get<1>(); }

  void add(reg_t reg) {
    apply<0>([&](UsedRegisters* used_regs) { used_regs->add(reg); });
  }

  void add(const IRInstruction* ptr) {
    apply<1>([&](UsedPointers* used_ptrs) { used_ptrs->add(ptr); });
  }

  void remove(reg_t reg) {
    apply<0>([&](UsedRegisters* used_regs) { used_regs->remove(reg); });
  }

  void remove(const IRInstruction* ptr) {
    apply<1>([&](UsedPointers* used_ptrs) { used_ptrs->remove(ptr); });
  }

  bool contains(reg_t reg) const { return get_used_registers().contains(reg); }

  bool contains(const IRInstruction* ptr) const {
    return get_used_pointers().contains(ptr);
  }
};

/*
 * This tracks which registers and which locally-allocated, non-escaping objects
 * get read from. It is essentially a liveness analysis that ignores
 * instructions which it can determine to have no observable side-effects.
 */
class UsedVarsFixpointIterator final
    : public sparta::MonotonicFixpointIterator<
          sparta::BackwardsFixpointIterationAdaptor<cfg::GraphInterface>,
          UsedVarsSet> {
 public:
  using NodeId = cfg::Block*;

  UsedVarsFixpointIterator(
      const local_pointers::FixpointIterator& pointers_fp_iter,
      const EffectSummaryMap& effect_summaries,
      const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
      const cfg::ControlFlowGraph& cfg);

  void analyze_node(const NodeId& block,
                    UsedVarsSet* used_vars) const override {
    TRACE(DEAD_CODE, 5, "Block B%u\n", block->id());
    for (auto it = block->rbegin(); it != block->rend(); ++it) {
      if (it->type == MFLOW_OPCODE) {
        analyze_instruction(it->insn, used_vars);
      }
    }
  }

  UsedVarsSet analyze_edge(
      const EdgeId&, const UsedVarsSet& exit_state_at_source) const override {
    return exit_state_at_source;
  }

  void analyze_instruction(const IRInstruction* insn,
                           UsedVarsSet* used_vars) const;

  // Returns true if a write to the object in obj_reg cannot be proven to be
  // unused.
  bool is_used_or_escaping_write(const local_pointers::Environment& env,
                                 const UsedVarsSet& used_vars,
                                 reg_t obj_reg) const;

  bool is_required(const IRInstruction* insn,
                   const UsedVarsSet& used_vars) const;

  UsedVarsSet get_used_vars_at_entry(const NodeId& block) const {
    return get_exit_state_at(block);
  }

  UsedVarsSet get_used_vars_at_exit(const NodeId& block) const {
    return get_entry_state_at(block);
  }

 private:
  std::unordered_map<const IRInstruction*, local_pointers::Environment>
      m_insn_env_map;
  const EffectSummaryMap& m_effect_summaries;
  const std::unordered_set<const DexMethod*>& m_non_overridden_virtuals;
};

std::vector<IRList::iterator> get_dead_instructions(
    const IRCode*, const UsedVarsFixpointIterator&);
