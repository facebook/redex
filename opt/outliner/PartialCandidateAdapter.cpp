/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PartialCandidateAdapter.h"

namespace outliner_impl {

PartialCandidateAdapter::PartialCandidateAdapter(OutlinerTypeAnalysis& ota,
                                                 PartialCandidate pc)
    : m_ota(ota), m_pc(std::move(pc)), m_insns([this]() {
        auto res = std::make_unique<std::unordered_set<IRInstruction*>>();
        std::function<void(const PartialCandidateNode&)> gather_insns;
        gather_insns = [&](const PartialCandidateNode& pcn) {
          res->insert(pcn.insns.begin(), pcn.insns.end());
          for (auto& p : pcn.succs) {
            gather_insns(*p.second);
          }
        };
        gather_insns(m_pc.root);
        return res;
      }) {}

void PartialCandidateAdapter::set_result(std::optional<reg_t> out_reg,
                                         const DexType* res_type) {
  m_out_reg = out_reg;
  m_res_type = res_type;
}

const type_inference::TypeEnvironment& PartialCandidateAdapter::get_type_env()
    const {
  auto insn = m_pc.root.insns.front();
  return m_ota.m_type_environments->at(insn);
}

const reaching_defs::Environment& PartialCandidateAdapter::get_rdef_env()
    const {
  auto insn = m_pc.root.insns.front();
  return m_ota.m_reaching_defs_environments->at(insn);
}

void PartialCandidateAdapter::gather_type_demands(
    const PartialCandidateNode& pcn,
    std::unordered_set<reg_t> regs_to_track,
    const std::function<bool(IRInstruction*, src_index_t)>& follow,
    std::unordered_set<const DexType*>* type_demands) const {
  for (size_t insn_idx = 0;
       insn_idx < pcn.insns.size() && !regs_to_track.empty();
       insn_idx++) {
    bool track_dest{false};
    auto insn = pcn.insns.at(insn_idx);
    for (size_t i = 0; i < insn->srcs_size(); i++) {
      if (!regs_to_track.count(insn->src(i))) {
        continue;
      }
      if (opcode::is_a_move(insn->opcode())) {
        track_dest = true;
        continue;
      }
      type_demands->insert(m_ota.get_type_demand(insn, i));
      if (follow(insn, i)) {
        track_dest = true;
      }
    }
    always_assert(!track_dest || insn->has_dest());
    if (insn->has_dest()) {
      if (track_dest) {
        regs_to_track.insert(insn->dest());
      } else {
        regs_to_track.erase(insn->dest());
      }
      if (insn->dest_is_wide()) {
        regs_to_track.erase(insn->dest() + 1);
      }
    }
  }
  if (pcn.succs.empty() && m_out_reg && regs_to_track.count(*m_out_reg)) {
    type_demands->insert(m_res_type);
  }
  for (auto& p : pcn.succs) {
    gather_type_demands(*p.second, regs_to_track, follow, type_demands);
  }
}

bool PartialCandidateAdapter::contains(IRInstruction* insn) const {
  return m_insns->count(insn);
}

} // namespace outliner_impl
