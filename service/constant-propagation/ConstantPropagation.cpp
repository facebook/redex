/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagation.h"

#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationTransform.h"

#include "ScopedCFG.h"
#include "Trace.h"
#include "Walkers.h"

namespace constant_propagation {

Transform::Stats ConstantPropagation::run(DexMethod* method,
                                          const XStoreRefs* xstores,
                                          const State& state) {
  if (method->get_code() == nullptr || method->rstate.no_optimizations()) {
    return Transform::Stats();
  }
  TRACE(CONSTP, 2, "Method: %s", SHOW(method));
  auto code = method->get_code();
  cfg::ScopedCFG cfg(code);

  TRACE(CONSTP, 5, "CFG: %s", SHOW(*cfg));
  Transform::Stats local_stats;
  {
    intraprocedural::FixpointIterator fp_iter(&state, *cfg,
                                              ConstantPrimitiveAnalyzer());
    fp_iter.run({});
    constant_propagation::Transform tf(m_config.transform, state);
    tf.apply(fp_iter, WholeProgramState(), code->cfg(), xstores,
             is_static(method), method->get_class(), method->get_proto());
    local_stats = tf.get_stats();
  }
  return local_stats;
}

Transform::Stats ConstantPropagation::run(const Scope& scope,
                                          const XStoreRefs* xstores,
                                          const State& state) {
  return walk::parallel::methods<Transform::Stats>(
      scope, [&](DexMethod* method) { return run(method, xstores, state); });
}
} // namespace constant_propagation
