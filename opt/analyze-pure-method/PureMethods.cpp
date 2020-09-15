/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PureMethods.h"
#include "LocalPointersAnalysis.h"
#include "PassManager.h"
#include "Purity.h"
#include "SideEffectSummary.h"
#include "SummarySerialization.h"
#include "Trace.h"
#include "Walkers.h"

bool AnalyzePureMethodsPass::analyze_and_check_pure_method_helper(
    IRCode* code) {
  auto& cfg = code->cfg();

  // MoveAwareFixpointIterator to see if any object accessed is parameter (OK)
  // or field (Not OK). SummaryBuilder will use this to decide if method can
  // be treated as pure. Pureness as defined in Purity.h.
  reaching_defs::MoveAwareFixpointIterator reaching_defs_iter(cfg);
  reaching_defs_iter.run({});

  local_pointers::FixpointIterator fp_iter(cfg);
  fp_iter.run({});

  auto side_effect_summary =
      side_effects::SummaryBuilder({},
                                   fp_iter,
                                   code,
                                   &reaching_defs_iter,
                                   /* analyze_external_reads */ true)
          .build();

  return side_effect_summary.is_pure();
}

void AnalyzePureMethodsPass::run_pass(DexStoresVector& stores,
                                      ConfigFiles&,
                                      PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  auto stats = analyze_and_set_pure_methods(scope);
  stats.report(mgr);
}

AnalyzePureMethodsPass::Stats
AnalyzePureMethodsPass::analyze_and_set_pure_methods(Scope& scope) {
  auto method_override_graph = method_override_graph::build_graph(scope);
  // Build all the CFGs
  walk::parallel::code(scope, [&](DexMethod*, IRCode& code) {
    code.build_cfg(/* editable */ false);
  });

  Stats stats = walk::parallel::methods<Stats>(scope, [&](DexMethod* method) {
    Stats stats;
    if (!method->get_code() || method->rstate.no_optimizations() ||
        method->rstate.immutable_getter()) {
      return stats;
    }
    auto code = method->get_code();
    bool is_method_pure = false;
    if (method->is_virtual()) {
      is_method_pure = process_base_and_overriding_methods(
          method_override_graph.get(), method, nullptr,
          /* ignore_methods_with_assumenosideeffects */ true,
          [&](DexMethod* overriding_method) {
            auto method_code = overriding_method->get_code();
            return analyze_and_check_pure_method_helper(method_code);
          });
    } else {
      is_method_pure = analyze_and_check_pure_method_helper(code);
    }

    if (!is_method_pure && method->rstate.pure_method()) {
      method->rstate.reset_pure_method();
      stats.number_of_pure_methods_invalidated++;
    }

    if (!is_method_pure) {
      return stats;
    }

    TRACE(CSE, 5, "[analyze_and_get_pure_methods] adding method %s\n",
          SHOW(method));
    stats.number_of_pure_methods_detected++;
    method->rstate.set_pure_method();
    return stats;
  });

  // Clear all the CFGs
  walk::parallel::code(scope,
                       [&](DexMethod*, IRCode& code) { code.clear_cfg(); });
  return stats;
}

void AnalyzePureMethodsPass::Stats::report(PassManager& mgr) const {
  mgr.incr_metric("number_of_pure_methods_detected ",
                  number_of_pure_methods_detected);
  mgr.incr_metric("number_of_pure_methods_invalidated ",
                  number_of_pure_methods_invalidated);
}

static AnalyzePureMethodsPass s_pass;
