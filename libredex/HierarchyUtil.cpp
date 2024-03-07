/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HierarchyUtil.h"

#include "RedexContext.h"
#include "Walkers.h"

namespace mog = method_override_graph;

namespace hierarchy_util {

NonOverriddenVirtuals::NonOverriddenVirtuals(
    const Scope& scope, const method_override_graph::Graph& override_graph) {
  walk::parallel::classes(scope, [&](DexClass* cls) {
    for (auto* method : cls->get_vmethods()) {
      if (!mog::any_overriding_methods(override_graph, method)) {
        m_non_overridden_virtuals.emplace(method);
      }
    }
  });
}

size_t NonOverriddenVirtuals::count(const DexMethod* method) const {
  if (method->is_external()) {
    return is_final(method) || is_final(type_class(method->get_class()));
  }
  return m_non_overridden_virtuals.count_unsafe(method);
}

} // namespace hierarchy_util
