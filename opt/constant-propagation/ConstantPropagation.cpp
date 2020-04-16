/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagation.h"

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"
#include "Walkers.h"

using namespace constant_propagation;

void ConstantPropagationPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles&,
                                       PassManager& mgr) {
  auto scope = build_class_scope(stores);

  auto stats =
      walk::parallel::methods<Transform::Stats>(scope, [&](DexMethod* method) {
        if (method->get_code() == nullptr) {
          return Transform::Stats();
        }

        TRACE(CONSTP, 2, "Method: %s", SHOW(method));
        auto& code = *method->get_code();
        code.build_cfg(/* editable */ false);
        auto& cfg = code.cfg();

        TRACE(CONSTP, 5, "CFG: %s", SHOW(cfg));
        intraprocedural::FixpointIterator fp_iter(cfg,
                                                  ConstantPrimitiveAnalyzer());
        fp_iter.run(ConstantEnvironment());
        constant_propagation::Transform tf(m_config.transform);
        return tf.apply(fp_iter, WholeProgramState(), &code);
      });

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
