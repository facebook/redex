/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResolveProguardAssumeValues.h"
#include "CFGMutation.h"
#include "ReachingDefinitions.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

void ResolveProguardAssumeValuesPass::run_pass(DexStoresVector& stores,
                                               ConfigFiles&,
                                               PassManager& mgr) {

  Scope scope = build_class_scope(stores);

  Stats stats = walk::parallel::methods<Stats>(scope, [&](DexMethod* method) {
    Stats stat;
    if (!method || !method->get_code()) {
      return stat;
    }
    auto code = method->get_code();
    return process_for_code(code);
  });
  stats.report(mgr);
  TRACE(PGR,
        2,
        "ResolveProguardAssumeValuesPass Stats: %ul",
        stats.method_return_values_changed);
}

ResolveProguardAssumeValuesPass::Stats
ResolveProguardAssumeValuesPass::process_for_code(IRCode* code) {

  Stats stat;
  cfg::ScopedCFG cfg(code);
  cfg::CFGMutation mutation(*cfg);

  for (auto* b : cfg->blocks()) {
    for (const auto& insn_it : ir_list::InstructionIterable{b}) {
      auto insn = insn_it.insn;

      // We consider static methods and methods that are not external.
      if (insn->opcode() != OPCODE_INVOKE_STATIC ||
          insn->get_method()->is_external()) {
        continue;
      }
      auto m = insn->get_method()->as_def();
      if (!m) {
        continue;
      }
      auto return_value = g_redex->get_return_value(m);
      if (!return_value) {
        continue;
      }

      if ((*return_value).value_type ==
          keep_rules::AssumeReturnValue::ValueNone) {
        continue;
      }
      auto it = cfg->find_insn(insn);
      auto next_it = std::next(it);
      if (!next_it.is_end() && (*return_value).value_type ==
                                   keep_rules::AssumeReturnValue::ValueBool) {
        auto next_insn = next_it->insn;

        if (next_insn->opcode() == OPCODE_MOVE_RESULT) {
          int val = (*return_value).value.v;
          IRInstruction* new_insn = new IRInstruction(OPCODE_CONST);
          new_insn->set_literal(val)->set_dest(next_insn->dest());

          TRACE(PGR, 5, "Changing:\n %s and %s", SHOW(insn), SHOW(next_insn));
          TRACE(PGR, 5, "TO:\n %s", SHOW(new_insn));
          mutation.replace(next_it, {new_insn});
          stat.method_return_values_changed++;
        }
      }
    }
  }
  mutation.flush();
  return stat;
}

static ResolveProguardAssumeValuesPass s_pass;
