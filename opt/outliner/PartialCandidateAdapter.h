/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Lazy.h"
#include "OutlinerTypeAnalysis.h"
#include "PartialCandidates.h"

namespace outliner_impl {

class PartialCandidateAdapter : public CandidateAdapter {
 public:
  PartialCandidateAdapter(OutlinerTypeAnalysis& ota, PartialCandidate pc);
  void set_result(std::optional<reg_t> out_reg, const DexType* res_type);
  const type_inference::TypeEnvironment& get_type_env() const override;
  const reaching_defs::Environment& get_rdef_env() const override;
  void gather_type_demands(
      std::unordered_set<reg_t> regs_to_track,
      const std::function<bool(IRInstruction*, src_index_t)>& follow,
      std::unordered_set<const DexType*>* type_demands) const override {
    gather_type_demands(m_pc.root, std::move(regs_to_track), follow,
                        type_demands);
  }
  bool contains(IRInstruction* insn) const override;

 private:
  void gather_type_demands(
      const PartialCandidateNode& pcn,
      std::unordered_set<reg_t> regs_to_track,
      const std::function<bool(IRInstruction*, src_index_t)>& follow,
      std::unordered_set<const DexType*>* type_demands) const;

  OutlinerTypeAnalysis& m_ota;
  PartialCandidate m_pc;
  std::optional<reg_t> m_out_reg;
  const DexType* m_res_type{nullptr};
  mutable Lazy<std::unordered_set<IRInstruction*>> m_insns;
};

} // namespace outliner_impl
