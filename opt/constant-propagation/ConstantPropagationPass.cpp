/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationPass.h"

#include "ConstantPropagation.h"
#include "PassManager.h"
#include "ScopedMetrics.h"
#include "Trace.h"
#include "Walkers.h"

using namespace constant_propagation;

void ConstantPropagationPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles&,
                                       PassManager& mgr) {
  auto scope = build_class_scope(stores);
  XStoreRefs xstores(stores);

  ConstantPropagation impl(m_config);
  auto stats = impl.run(scope, &xstores);

  ScopedMetrics sm(mgr);
  stats.log_metrics(sm, /* with_scope= */ false);

  TRACE(CONSTP, 1, "num_branch_propagated: %zu", stats.branches_removed);
  TRACE(CONSTP,
        1,
        "num_moves_replaced_by_const_loads: %zu",
        stats.materialized_consts);
  TRACE(CONSTP, 1, "num_throws: %zu", stats.throws);
}

static ConstantPropagationPass s_pass;
