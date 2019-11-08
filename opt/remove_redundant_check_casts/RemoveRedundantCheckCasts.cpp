/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveRedundantCheckCasts.h"

#include "CheckCastAnalysis.h"
#include "DexClass.h"
#include "PassManager.h"
#include "Walkers.h"

namespace check_casts {

size_t remove_redundant_check_casts(DexMethod* method) {
  if (!method || !method->get_code()) {
    return 0;
  }

  auto* code = method->get_code();
  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  impl::CheckCastAnalysis analysis(method);
  auto casts = analysis.collect_redundant_checks_replacement();
  for (const auto& cast : casts) {
    auto it = cfg.find_insn(cast.insn, cast.block);
    boost::optional<IRInstruction*> replacement = cast.replacement;
    if (replacement) {
      cfg.replace_insn(it, *replacement);
    } else {
      cfg.remove_insn(it);
    }
  }

  code->clear_cfg();
  return casts.size();
}

void RemoveRedundantCheckCastsPass::run_pass(DexStoresVector& stores,
                                             ConfigFiles&,
                                             PassManager& mgr) {
  auto scope = build_class_scope(stores);

  size_t num_redundant_check_casts =
      walk::parallel::methods<size_t>(scope, [&](DexMethod* method) {
        return remove_redundant_check_casts(method);
      });

  mgr.set_metric("num_redundant_check_casts", num_redundant_check_casts);
}

static RemoveRedundantCheckCastsPass s_pass;

} // namespace check_casts
