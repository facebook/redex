/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Lazy.h"
#include "LiveRange.h"
#include "OutlinerTypeAnalysis.h"
#include "PartialCandidates.h"
#include "ReducedControlFlow.h"

namespace outliner_impl {

class ReducedCFGClosureAdapter : public CandidateAdapter {
 public:
  ReducedCFGClosureAdapter(
      OutlinerTypeAnalysis& ota,
      IRInstruction* first_insn,
      Lazy<std::unordered_map<IRInstruction*,
                              const method_splitting_impl::ReducedBlock*>>&
          insns,
      const std::unordered_set<const method_splitting_impl::ReducedBlock*>&
          reduced_components,
      Lazy<live_range::DefUseChains>& def_uses);
  const type_inference::TypeEnvironment& get_type_env() const override;
  const reaching_defs::Environment& get_rdef_env() const override;
  void gather_type_demands(
      std::unordered_set<reg_t> regs_to_track,
      const std::function<bool(IRInstruction*, src_index_t)>& follow,
      std::unordered_set<const DexType*>* type_demands) const override;
  bool contains(IRInstruction* insn) const override;
  sparta::PatriciaTreeSet<IRInstruction*> get_defs(reg_t reg) const;

 private:
  OutlinerTypeAnalysis& m_ota;
  IRInstruction* m_first_insn;
  const std::unordered_set<const method_splitting_impl::ReducedBlock*>&
      m_reduced_components;
  Lazy<std::unordered_map<IRInstruction*,
                          const method_splitting_impl::ReducedBlock*>>& m_insns;
  Lazy<live_range::DefUseChains>& m_def_uses;
};

} // namespace outliner_impl
