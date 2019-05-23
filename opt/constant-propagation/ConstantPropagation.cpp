/**
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

void ConstantPropagationPass::configure_pass(const JsonWrapper& jw) {
  jw.get("replace_moves_with_consts",
         true,
         m_config.transform.replace_moves_with_consts);
}

void ConstantPropagationPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles&,
                                       PassManager& mgr) {
  auto scope = build_class_scope(stores);

  auto stats = walk::parallel::reduce_methods<Transform::Stats>(
      scope,
      [&](DexMethod* method) {
        if (method->get_code() == nullptr) {
          return Transform::Stats();
        }

        TRACE(CONSTP, 2, "Method: %s\n", SHOW(method));
        auto& code = *method->get_code();
        code.build_cfg(/* editable */ false);
        auto& cfg = code.cfg();

        TRACE(CONSTP, 5, "CFG: %s\n", SHOW(cfg));
        intraprocedural::FixpointIterator fp_iter(cfg,
                                                  ConstantPrimitiveAnalyzer());
        fp_iter.run(ConstantEnvironment());
        constant_propagation::Transform tf(m_config.transform);
        return tf.apply(fp_iter, WholeProgramState(), &code);
      },

      [](Transform::Stats a, Transform::Stats b) { // reducer
        return a + b;
      });

  mgr.incr_metric("num_branch_propagated", stats.branches_removed);
  mgr.incr_metric("num_materialized_consts", stats.materialized_consts);

  TRACE(CONSTP, 1, "num_branch_propagated: %d\n", stats.branches_removed);
  TRACE(CONSTP,
        1,
        "num_moves_replaced_by_const_loads: %d\n",
        stats.materialized_consts);
}

static ConstantPropagationPass s_pass;
