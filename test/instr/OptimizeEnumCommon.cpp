/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "OptimizeEnumCommon.h"

#include "MatchFlow.h"

std::set<BranchCase> collect_const_branch_cases(DexMethodRef* method_ref) {
  auto* method = static_cast<DexMethod*>(method_ref);
  method->balloon();

  auto* code = method->get_code();
  code->build_cfg();
  cfg::ControlFlowGraph& cfg = code->cfg();
  cfg.calculate_exit_block();

  mf::flow_t f;

  auto uniq = mf::alias | mf::unique;
  auto forall = mf::alias | mf::forall;

  auto value = f.insn(m::aget_() || m::const_() || m::invoke_virtual_());
  auto kase = f.insn(m::const_());

  auto cmp_switch = f.insn(m::switch_()).src(0, value, forall);
  auto cmp_if_to_zero =
      f.insn(m::if_eqz_() || m::if_nez_()).src(0, value, forall);
  auto cmp_if_src0 = f.insn(m::if_eq_() || m::if_ne_())
                         .src(0, value, forall)
                         .src(1, kase, uniq);
  auto cmp_if_src1 = f.insn(m::if_eq_() || m::if_ne_())
                         .src(0, kase, forall)
                         .src(1, value, uniq);

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
    const src_index_t value_src =
        static_cast<const src_index_t>(cmp_location == cmp_if_src1);

    for (auto* insn_cmp : res.matching(cmp_location)) {
      auto cmp_it = cfg.find_insn(insn_cmp);
      always_assert(!cmp_it.is_end());

      // Determine which source instruction actually matched. In our tests
      // we expect to see either a unique AGET or INVOKE_VIRTUAL for most
      // cases. If the switch includes a nullable Kotlin enum then we may
      // see a non-unique source that includes a CONST of -1.
      //
      // Anything else is unexpected.
      auto insn_value_range = res.matching(cmp_location, insn_cmp, value_src);
      always_assert(insn_value_range);

      const IRInstruction* insn_value;
      bool has_const_value;
      if (const IRInstruction* insn_value_unique = insn_value_range.unique()) {
        insn_value = insn_value_unique;
        has_const_value = false;
      } else {
        auto range_iter = insn_value_range.begin();
        const IRInstruction* first_insn_value = *range_iter++;
        const IRInstruction* second_insn_value = *range_iter++;
        always_assert(range_iter == insn_value_range.end());

        const IRInstruction* insn_const;
        if (first_insn_value->opcode() == OPCODE_CONST) {
          insn_value = second_insn_value;
          insn_const = first_insn_value;
        } else if (second_insn_value->opcode() == OPCODE_CONST) {
          insn_value = first_insn_value;
          insn_const = second_insn_value;
        } else {
          always_assert(false);
        }

        always_assert(insn_const->get_literal() == -1);
        has_const_value = true;
      }

      BranchSource branch_source;
      if (insn_value->opcode() == OPCODE_AGET) {
        branch_source = has_const_value ? BranchSource::ArrayGetOrConstMinus1
                                        : BranchSource::ArrayGet;
      } else if (insn_value->opcode() == OPCODE_INVOKE_VIRTUAL) {
        branch_source = has_const_value ? BranchSource::VirtualCallOrConstMinus1
                                        : BranchSource::VirtualCall;
      } else {
        always_assert(false);
      }

      // And then determine which comparisons are being made.
      if (cmp_location == cmp_switch) {
        for (const auto& succ : cmp_it.block()->succs()) {
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
