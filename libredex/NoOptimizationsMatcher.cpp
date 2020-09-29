/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NoOptimizationsMatcher.h"
#include "Walkers.h"

namespace keep_rules {

void process_no_optimizations_rules(
    const std::unordered_set<DexType*>& no_optimizations_annos,
    const Scope& scope) {
  auto match = m::any_annos<DexMethod>(
      m::as_type<DexAnnotation>(m::in<DexType>(&no_optimizations_annos)));

  walk::parallel::methods(scope, [&](DexMethod* method) {
    if (match.matches(method)) {
      method->rstate.set_no_optimizations();
    }
  });
}

} // namespace keep_rules
