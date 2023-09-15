/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "MethodOverrideGraph.h"

namespace hierarchy_util {

/*
 * Identifies all non-overridden virtual methods in scope, plus methods from
 * external classes. The external classes will be included even if they are not
 * in the input Scope parameter.
 */
class NonOverriddenVirtuals {
 public:
  NonOverriddenVirtuals(const Scope& scope,
                        const method_override_graph::Graph& override_graph);
  explicit NonOverriddenVirtuals(const Scope& scope)
      : NonOverriddenVirtuals(scope,
                              *method_override_graph::build_graph(scope)) {}

  size_t count(const DexMethod*) const;

 private:
  ConcurrentSet<const DexMethod*> m_non_overridden_virtuals;
};

} // namespace hierarchy_util
