/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

namespace used_vars {

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
class FixpointIterator final
    : public ir_analyzer::BaseBackwardsIRAnalyzer<UsedVarsSet> {
 public:
  FixpointIterator(const local_pointers::FixpointIterator& pointers_fp_iter,
                   side_effects::InvokeToSummaryMap invoke_to_summary_map,
                   const cfg::ControlFlowGraph& cfg);

  void analyze_instruction(IRInstruction* insn,
                           UsedVarsSet* used_vars) const override;

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
  side_effects::InvokeToSummaryMap m_invoke_to_summary_map;
};

std::vector<IRList::iterator> get_dead_instructions(const IRCode&,
                                                    const FixpointIterator&);

} // namespace used_vars
