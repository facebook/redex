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

namespace constant_propagation {

Transform::Stats ConstantPropagation::run(DexMethod* method) {
  if (method->get_code() == nullptr) {
    return Transform::Stats();
  }
  TRACE(CONSTP, 2, "Method: %s", SHOW(method));
  auto code = method->get_code();
  code->build_cfg(/* editable */ false);
  auto& cfg = code->cfg();

  TRACE(CONSTP, 5, "CFG: %s", SHOW(cfg));
  intraprocedural::FixpointIterator fp_iter(cfg, ConstantPrimitiveAnalyzer());
  fp_iter.run(ConstantEnvironment());
  constant_propagation::Transform tf(m_config.transform);
  return tf.apply(fp_iter, WholeProgramState(), code);
}

Transform::Stats ConstantPropagation::run(const Scope& scope) {
  return walk::parallel::methods<Transform::Stats>(
      scope, [&](DexMethod* method) { return run(method); });
}
} // namespace constant_propagation
