/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ResolveProguardAssumeValues.h"

#include "CFGMutation.h"
#include "ProguardConfiguration.h"
#include "ReachingDefinitions.h"
#include "RedexContext.h"
#include "Resolver.h"
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
    always_assert(method->get_code()->editable_cfg_built());
    auto& cfg = method->get_code()->cfg();
    return process_for_code(cfg);
  });
  stats.report(mgr);
  TRACE(PGR,
        2,
        "ResolveProguardAssumeValuesPass return values changed: %zu",
        stats.method_return_values_changed);
  TRACE(PGR,
        2,
        "ResolveProguardAssumeValuesPass field values changed: %zu",
        stats.field_values_changed);
}

ResolveProguardAssumeValuesPass::Stats
ResolveProguardAssumeValuesPass::process_for_code(cfg::ControlFlowGraph& cfg) {

  Stats stat;
  cfg::CFGMutation mutation(cfg);

  auto ii = InstructionIterable(cfg);
  for (auto insn_it = ii.begin(); insn_it != ii.end(); ++insn_it) {
    auto insn = insn_it->insn;
    switch (insn->opcode()) {
    case OPCODE_SGET_BOOLEAN: {
      auto field = resolve_field(insn->get_field());
      auto field_value = g_redex->get_field_value(field);
      if (!field_value ||
          field_value->value_type != keep_rules::AssumeReturnValue::ValueBool) {
        break;
      }

      auto move_result_it = cfg.move_result_of(insn_it);
      if (!move_result_it.is_end()) {
        auto move_insn = move_result_it->insn;
        int val = field_value->value.v;
        IRInstruction* new_insn = new IRInstruction(OPCODE_CONST);
        new_insn->set_literal(val)->set_dest(move_insn->dest());

        TRACE(PGR, 5, "Changing:\n %s and %s", SHOW(insn), SHOW(move_insn));
        TRACE(PGR, 5, "TO:\n %s", SHOW(new_insn));
        mutation.replace(move_result_it, {new_insn});
        stat.field_values_changed++;
      }
      break;
    }
    case OPCODE_INVOKE_STATIC: {
      auto m = insn->get_method()->as_def();
      // We consider static methods and methods that are not external.
      if (!m || insn->get_method()->is_external()) {
        break;
      }
      auto return_value = g_redex->get_return_value(m);
      if (!return_value || return_value->value_type !=
                               keep_rules::AssumeReturnValue::ValueBool) {
        break;
      }

      auto move_result_it = cfg.move_result_of(insn_it);
      if (!move_result_it.is_end()) {
        auto move_insn = move_result_it->insn;
        int val = (*return_value).value.v;
        IRInstruction* new_insn = new IRInstruction(OPCODE_CONST);
        new_insn->set_literal(val)->set_dest(move_insn->dest());

        TRACE(PGR, 5, "Changing:\n %s and %s", SHOW(insn), SHOW(move_insn));
        TRACE(PGR, 5, "TO:\n %s", SHOW(new_insn));
        mutation.replace(move_result_it, {new_insn});
        stat.method_return_values_changed++;
      }
      break;
    }
    default:
      break;
    } // switch
  }
  mutation.flush();
  return stat;
}

static ResolveProguardAssumeValuesPass s_pass;
