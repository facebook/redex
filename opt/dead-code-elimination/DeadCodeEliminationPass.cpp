/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DeadCodeEliminationPass.h"

#include "DexUtil.h"
#include "LocalPointersAnalysis.h"
#include "SummarySerialization.h"
#include "Transform.h"
#include "VirtualScope.h"
#include "Walkers.h"

/*
 * This pass tries to identify writes to registers and objects that never get
 * read from. Modeling dead object field writes is particularly useful in
 * conjunction with RemoveUnreadFieldsPass. Suppose we have an unused field
 * Foo.x:
 *
 *   new-instance v0 LFoo;
 *   invoke-direct {v0} LFoo;.<init>()V
 *   sput-object v0 LBar;.x:LFoo; # RMUF will remove this
 *
 * If we can determine that Foo's constructor does not modify anything
 * outside of its `this` argument, we will be able to remove the invoke-direct
 * call as well as the new-instance instruction.
 *
 * In contrast, LocalDce can only identify unused writes to registers -- it
 * knows nothing about objects. The trade-off is that this is takes much longer
 * to run.
 */

namespace ptrs = local_pointers;

std::unique_ptr<UsedVarsFixpointIterator> DeadCodeEliminationPass::analyze(
    const EffectSummaryMap& effect_summaries,
    const std::unordered_set<const DexMethod*>& non_overridden_virtuals,
    IRCode& code) {
  auto& cfg = code.cfg();
  cfg.calculate_exit_block();
  // First we do a forwards analysis to determine which registers hold
  // locally-allocated pointers with non-escaping pointees..
  ptrs::FixpointIterator pointers_fp_iter(cfg);
  pointers_fp_iter.run(ptrs::Environment());

  // Then we use that information as part of the backwards used vars analysis
  // to determine which locally-allocated pointers are being used.
  auto used_vars_fp_iter = std::make_unique<UsedVarsFixpointIterator>(
      pointers_fp_iter, effect_summaries, non_overridden_virtuals, cfg);
  used_vars_fp_iter->run(UsedVarsSet());

  return used_vars_fp_iter;
}

void DeadCodeEliminationPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles&,
                                       PassManager&) {
  auto scope = build_class_scope(stores);
  walk::parallel::code(
      scope, [&](const DexMethod* method, IRCode& code) { code.build_cfg(/* editable */ false); });

  auto non_overridden_virtuals = find_non_overridden_virtuals(scope);

  EffectSummaryMap effect_summaries;
  if (m_external_summaries_file) {
    std::ifstream file_input(*m_external_summaries_file);
    summary_serialization::read(file_input, &effect_summaries);
  }
  summarize_all_method_effects(
      scope, non_overridden_virtuals, &effect_summaries);

  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    auto used_vars_fp_iter =
        analyze(effect_summaries, non_overridden_virtuals, code);

    TRACE(DEAD_CODE, 5, "Transforming %s\n", SHOW(method));
    TRACE(DEAD_CODE, 5, "Before:\n%s\n", SHOW(code.cfg()));
    auto dead_instructions = get_dead_instructions(&code, *used_vars_fp_iter);
    for (auto dead : dead_instructions) {
      code.remove_opcode(dead);
    }
    transform::remove_unreachable_blocks(&code);
    TRACE(DEAD_CODE, 5, "After:\n%s\n", SHOW(&code));
  });
}

static DeadCodeEliminationPass s_pass;
