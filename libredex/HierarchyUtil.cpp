/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "HierarchyUtil.h"

#include "MethodOverrideGraph.h"

namespace mog = method_override_graph;

namespace hierarchy_util {

std::unordered_set<const DexMethod*> find_non_overridden_virtuals(
    const Scope& scope) {
  auto override_graph = mog::build_graph(scope);
  std::unordered_set<const DexMethod*> non_overridden_virtuals;
  g_redex->walk_type_class([&](const DexType*, const DexClass* cls) {
    if (!cls->is_external()) {
      for (auto* method : cls->get_vmethods()) {
        const auto& overrides =
            mog::get_overriding_methods(*override_graph, method);
        if (overrides.empty()) {
          non_overridden_virtuals.emplace(method);
        }
      }
    } else {
      for (auto* method : cls->get_vmethods()) {
        if (is_final(cls) || is_final(method)) {
          non_overridden_virtuals.emplace(method);
        }
      }
    }
  });
  return non_overridden_virtuals;
}

} // namespace hierarchy_util
