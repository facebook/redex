/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagation.h"
#include "ConstantPropagationImpl.h"

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "Walkers.h"

using namespace constant_propagation;

void ConstantPropagationPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles&,
                                       PassManager& mgr) {
  auto scope = build_class_scope(stores);

  ConstantPropagation impl(m_config);
  auto stats = impl.run(scope);

  mgr.incr_metric("num_branch_propagated", stats.branches_removed);
  mgr.incr_metric("num_materialized_consts", stats.materialized_consts);
  mgr.incr_metric("num_throws", stats.throws);

  TRACE(CONSTP, 1, "num_branch_propagated: %d", stats.branches_removed);
  TRACE(CONSTP,
        1,
        "num_moves_replaced_by_const_loads: %d",
        stats.materialized_consts);
  TRACE(CONSTP, 1, "num_throws: %d", stats.throws);
}

static ConstantPropagationPass s_pass;
