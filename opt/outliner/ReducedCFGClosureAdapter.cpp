/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReducedCFGClosureAdapter.h"

namespace outliner_impl {

using namespace method_splitting_impl;

ReducedCFGClosureAdapter::ReducedCFGClosureAdapter(
    OutlinerTypeAnalysis& ota,
    IRInstruction* first_insn,
    Lazy<UnorderedMap<IRInstruction*,
                      const method_splitting_impl::ReducedBlock*>>& insns,
    const UnorderedSet<const ReducedBlock*>& reduced_components,
    Lazy<live_range::DefUseChains>& def_uses)
    : m_ota(ota),
      m_first_insn(first_insn),
      m_reduced_components(reduced_components),
      m_insns(insns),
      m_def_uses(def_uses) {}

const type_inference::TypeEnvironment& ReducedCFGClosureAdapter::get_type_env()
    const {
  return m_ota.m_type_environments->at(m_first_insn);
}

const reaching_defs::Environment& ReducedCFGClosureAdapter::get_rdef_env()
    const {
  return m_ota.m_reaching_defs_environments->at(m_first_insn);
}

sparta::PatriciaTreeSet<IRInstruction*> ReducedCFGClosureAdapter::get_defs(
    reg_t reg) const {
  return get_rdef_env().get(reg).elements();
}

void ReducedCFGClosureAdapter::gather_type_demands(
    UnorderedSet<reg_t> regs_to_track,
    const std::function<bool(IRInstruction*, src_index_t)>& follow,
    UnorderedSet<const DexType*>* type_demands) const {
  std::queue<IRInstruction*> workqueue;
  UnorderedSet<IRInstruction*> visited;
  auto irdef_env =
      m_ota.m_immediate_reaching_defs_environments->at(m_first_insn);
  for (auto reg : UnorderedIterable(regs_to_track)) {
    auto defs = irdef_env.get(reg).elements();
    for (auto* def : defs) {
      workqueue.push(def);
    }
  }
  while (!workqueue.empty()) {
    auto* def = workqueue.front();
    workqueue.pop();
    if (!visited.insert(def).second) {
      continue;
    }
    for (auto& use : UnorderedIterable((*m_def_uses)[def])) {
      const auto* reduced_component = m_insns->at(use.insn);
      if (!m_reduced_components.count(reduced_component)) {
        continue;
      }
      if (opcode::is_a_move(use.insn->opcode())) {
        workqueue.push(use.insn);
        continue;
      }
      if (opcode::is_a_return(use.insn->opcode())) {
        type_demands->insert(m_ota.m_method->get_proto()->get_rtype());
        continue;
      }
      type_demands->insert(m_ota.get_type_demand(use.insn, use.src_index));
      if (follow(use.insn, use.src_index)) {
        workqueue.push(use.insn);
      }
    }
  }
}

bool ReducedCFGClosureAdapter::contains(IRInstruction* insn) const {
  return m_first_insn == insn || m_reduced_components.count(m_insns->at(insn));
}

} // namespace outliner_impl
