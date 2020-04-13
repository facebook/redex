/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveRedundantCheckCasts.h"

#include "CheckCastAnalysis.h"
#include "CheckCastTransform.h"
#include "DexClass.h"
#include "PassManager.h"
#include "Walkers.h"

namespace check_casts {

impl::Stats remove_redundant_check_casts(DexMethod* method) {
  if (!method || !method->get_code()) {
    return impl::Stats{};
  }

  auto* code = method->get_code();
  code->build_cfg(/* editable */ true);
  impl::CheckCastAnalysis analysis(method);
  auto casts = analysis.collect_redundant_checks_replacement();
  auto stats = impl::apply(method, casts);

  code->clear_cfg();
  return stats;
}

void RemoveRedundantCheckCastsPass::run_pass(DexStoresVector& stores,
                                             ConfigFiles&,
                                             PassManager& mgr) {
  auto scope = build_class_scope(stores);

  auto stats =
      walk::parallel::methods<impl::Stats>(scope, [&](DexMethod* method) {
        return remove_redundant_check_casts(method);
      });

  mgr.set_metric("num_removed_casts", stats.removed_casts);
  mgr.set_metric("num_replaced_casts", stats.replaced_casts);
}

static RemoveRedundantCheckCastsPass s_pass;

} // namespace check_casts
