/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "DexClass.h"
#include "DexStore.h"

/*
 * This module builds a DAG that enables us to quickly answer the following
 * question: Given a method reference, what is the set of methods that it could
 * possibly resolve to at runtime?
 */
namespace method_override_graph {

class Graph;

/*
 * Slow-ish; users should build the graph once and cache it somewhere.
 */
std::unique_ptr<const Graph> build_graph(const Scope&);

/*
 * Returns all the methods that override :method. The set does *not* include
 * :method itself.
 * While represented as a vector, the result is conceptually an unordered sets.
 */
std::vector<const DexMethod*> get_overriding_methods(
    const Graph& graph,
    const DexMethod* method,
    bool include_interfaces = false);

/*
 * Returns all the methods that are overridden by :method. The set does *not*
 * include the :method itself.
 * While represented as a vector, the result is conceptually an unordered sets.
 */
std::vector<const DexMethod*> get_overridden_methods(
    const Graph& graph,
    const DexMethod* method,
    bool include_interfaces = false);

/*
 * Whether a method overrides or is overridden by any other method.
 *
 * Abstract methods are always true virtuals, even if they lack an
 * implementation.
 */
bool is_true_virtual(const Graph& graph, const DexMethod* method);

/*
 * Return all non-true-virtuals in scope.
 */
std::unordered_set<DexMethod*> get_non_true_virtuals(const Graph& graph,
                                                     const Scope& scope);

/*
 * The `children` edges point to the overriders / implementors of the current
 * Node's method.
 */
struct Node {
  std::unordered_set<const DexMethod*> parents;
  std::unordered_set<const DexMethod*> children;
};

class Graph {
 public:
  const Node& get_node(const DexMethod* method) const;

  const ConcurrentMap<const DexMethod*, Node>& nodes() const { return m_nodes; }

  void add_edge(const DexMethod* overridden, const DexMethod* overriding);

  void dump(std::ostream&) const;

 private:
  static Node empty_node;
  ConcurrentMap<const DexMethod*, Node> m_nodes;
};

} // namespace method_override_graph
