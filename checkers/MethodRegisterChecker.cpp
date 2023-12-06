/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodRegisterChecker.h"

#include "DexClass.h"
#include "DexOpcode.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "Interference.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Walkers.h"

namespace redex_properties {

/**
 * Return the end of param register frame, to check there is no other
 * instructions using register bigger than param registers.
 * Also check in this method that param registers are continuous and
 * crash program if not.
 */
reg_t get_param_end(const char* property_name,
                    const cfg::ControlFlowGraph& cfg,
                    DexMethod* method) {
  auto params = cfg.get_param_instructions();
  if (params.empty()) {
    return regalloc::max_unsigned_value(16);
  }
  auto param_ops = InstructionIterable(params);
  auto it = param_ops.begin();
  reg_t prev_reg = it->insn->dest();
  reg_t spacing = it->insn->dest_is_wide() ? 2 : 1;
  ++it;
  for (; it != param_ops.end(); ++it) {
    // check that the param registers are contiguous
    always_assert_log(
        (prev_reg + spacing) == it->insn->dest(),
        "[%s] Param registers are not contiguous for method %s:\n%s",
        property_name,
        SHOW(method),
        SHOW(params));
    spacing = it->insn->dest_is_wide() ? 2 : 1;
    prev_reg = it->insn->dest();
  }
  return prev_reg + spacing - 1;
}

void MethodRegisterChecker::run_checker(DexStoresVector& stores,
                                        ConfigFiles& /* conf */,
                                        PassManager& /* mgr */,
                                        bool established) {
  if (!established) {
    return;
  }
  const auto& scope = build_class_scope(stores);
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    cfg::ScopedCFG cfg(&code);
    // 1. Load param's registers are at the end of register frames.
    reg_t max_param_reg =
        get_param_end(get_name(get_property()), code.cfg(), method);
    auto ii = cfg::InstructionIterable(*cfg);
    for (auto it = ii.begin(); it != ii.end(); ++it) {
      // Checking several things for each method:
      auto insn = it->insn;

      // 2. dest register is below max param reg and register limit.
      if (insn->has_dest()) {
        always_assert_log(
            insn->dest() <= max_param_reg,
            "[%s] Instruction %s refers to a register (v%u) > param"
            " registers (%u) in method %s\n",
            get_name(get_property()),
            SHOW(insn),
            insn->dest(),
            max_param_reg,
            SHOW(method));
        size_t max_dest_reg = regalloc::max_unsigned_value(
            regalloc::interference::dest_bit_width(it));
        always_assert_log(
            insn->dest() <= max_dest_reg,
            "[%s] Instruction %s refers to a register (v%u) > max dest"
            " register (%zu) in method %s\n",
            get_name(get_property()),
            SHOW(insn),
            insn->dest(),
            max_dest_reg,
            SHOW(method));
      }
      bool is_range = false;
      if (opcode::has_range_form(insn->opcode())) {
        insn->denormalize_registers();
        is_range = needs_range_conversion(insn);
        if (is_range) {
          // 3. invoke-range's registers are continuous
          always_assert_log(insn->has_contiguous_range_srcs_denormalized(),
                            "[%s] Instruction %s has non-contiguous srcs in "
                            "method %s.\n",
                            get_name(get_property()),
                            SHOW(insn),
                            SHOW(method));

          // 4. No overly large range instructions.
          auto size = insn->srcs_size();
          // From DexInstruction::set_range_size;
          always_assert_log(
              dex_opcode::format(opcode::range_version(insn->opcode())) ==
                      FMT_f5rc ||
                  size == (size & 0xff),
              "[%s] Range instruction %s takes too much src size in method "
              "%s.\n",
              get_name(get_property()),
              SHOW(insn),
              SHOW(method));
        }
        insn->normalize_registers();
      }
      // 5. All src registers are below max param reg and register limits.
      for (size_t i = 0; i < insn->srcs_size(); ++i) {
        always_assert_log(
            insn->src(i) <= max_param_reg,
            "[%s] Instruction %s refers to a register (v%u) > param"
            " registers (%u) in method %s\n",
            get_name(get_property()),
            SHOW(insn),
            insn->src(i),
            max_param_reg,
            SHOW(method));
        if (!is_range) {
          auto max_src_reg = regalloc::interference::max_value_for_src(
              insn, i, insn->src_is_wide(i));
          always_assert_log(
              insn->src(i) <= max_src_reg,
              "[%s] Instruction %s refers to a register (v%u) > max src"
              " registers (%u) in method %s\n",
              get_name(get_property()),
              SHOW(insn),
              insn->src(i),
              max_src_reg,
              SHOW(method));
        }
      }
    }
  });
}

} // namespace redex_properties

namespace {
static redex_properties::MethodRegisterChecker s_checker;
} // namespace
