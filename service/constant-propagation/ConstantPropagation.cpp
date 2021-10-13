/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
                                          XStoreRefs* xstores) {
  if (method->get_code() == nullptr) {
    return Transform::Stats();
  }
  TRACE(CONSTP, 2, "Method: %s", SHOW(method));
  auto code = method->get_code();
  cfg::ScopedCFG cfg(code);

  TRACE(CONSTP, 5, "CFG: %s", SHOW(*cfg));
  Transform::Stats local_stats;
  {
    intraprocedural::FixpointIterator fp_iter(*cfg,
                                              ConstantPrimitiveAnalyzer());
    fp_iter.run({});
    constant_propagation::Transform tf(m_config.transform);
    tf.apply(fp_iter, WholeProgramState(), code->cfg(), xstores,
             is_static(method), method->get_class(), method->get_proto());
    local_stats = tf.get_stats();
  }
  return local_stats;
}

Transform::Stats ConstantPropagation::run(const Scope& scope,
                                          XStoreRefs* xstores) {
  return walk::parallel::methods<Transform::Stats>(
      scope, [&](DexMethod* method) { return run(method, xstores); });
}
} // namespace constant_propagation
