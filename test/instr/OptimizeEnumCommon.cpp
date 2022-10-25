/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptimizeEnumCommon.h"

#include "MatchFlow.h"

std::set<BranchCase> collect_const_branch_cases(DexMethodRef* method_ref) {
  auto method = static_cast<DexMethod*>(method_ref);
  method->balloon();

  auto* code = method->get_code();
  code->build_cfg(/* editable */ true);
  cfg::ControlFlowGraph& cfg = code->cfg();
  cfg.calculate_exit_block();

  mf::flow_t f;

  auto uniq = mf::alias | mf::unique;
  auto value = f.insn(m::aget_() || m::invoke_virtual_());
  auto kase = f.insn(m::const_());

  auto cmp_switch = f.insn(m::switch_()).src(0, value, uniq);
  auto cmp_if_to_zero =
      f.insn(m::if_eqz_() || m::if_nez_()).src(0, value, uniq);
  auto cmp_if_src0 =
      f.insn(m::if_eq_() || m::if_ne_()).src(0, value, uniq).src(1, kase, uniq);
  auto cmp_if_src1 =
      f.insn(m::if_eq_() || m::if_ne_()).src(0, kase, uniq).src(1, value, uniq);

  const auto cmp_locations = {
      cmp_switch,
      cmp_if_to_zero,
      cmp_if_src0,
      cmp_if_src1,

  };
  auto res = f.find(cfg, cmp_locations);

  std::set<BranchCase> branch_cases;
  for (auto cmp_location : cmp_locations) {
    // The lookup value goes to src 0 for every cmp instruction
    // we are checking, except for the flipped cmp_if_src1 set.
    const src_index_t value_src = cmp_location == cmp_if_src1;

    for (auto* insn_cmp : res.matching(cmp_location)) {
      auto cmp_it = cfg.find_insn(insn_cmp);
      always_assert(!cmp_it.is_end());

      // Determine which source instruction actually matched.
      const IRInstruction* insn_value =
          res.matching(cmp_location, insn_cmp, value_src).unique();
      always_assert(insn_value);

      BranchSource branch_source;
      if (insn_value->opcode() == OPCODE_AGET) {
        branch_source = BranchSource::ArrayGet;
      } else if (insn_value->opcode() == OPCODE_INVOKE_VIRTUAL) {
        branch_source = BranchSource::VirtualCall;
      } else {
        always_assert(false);
      }

      // And then determine which comparisons are being made.
      if (cmp_location == cmp_switch) {
        for (auto& succ : cmp_it.block()->succs()) {
          if (succ->case_key()) {
            branch_cases.insert({branch_source, *succ->case_key()});
          }
        }
      } else if (cmp_location == cmp_if_to_zero) {
        branch_cases.insert({branch_source, 0});
      } else {
        const src_index_t kase_src = 1 - value_src;

        const IRInstruction* insn_kase =
            res.matching(cmp_location, insn_cmp, kase_src).unique();
        always_assert(insn_kase);

        branch_cases.insert({branch_source, insn_kase->get_literal()});
      }
    }
  }

  return branch_cases;
}
