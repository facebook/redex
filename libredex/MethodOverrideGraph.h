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
    bool include_interfaces = false,
    const DexType* base_type = nullptr);

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
 * When a class method implements interface methods only in a subclass of the
 * method's declaring class, then we need to track additional information.
 */
struct OtherInterfaceImplementations {
  // The set of immediately implemented interface methods.
  std::unordered_set<const DexMethod*> parents;
  // The set of the classes for which the current method implements those
  // interface methods for the first time.
  std::vector<const DexClass*> classes;
};

/*
 * The `children` edges point to the overriders / implementors of the current
 * Node's method.
 */
struct Node {
  // The set of immediately overridden / implemented methods.
  std::vector<const DexMethod*> parents;
  // The set of immediately overriding / implementing methods.
  std::vector<const DexMethod*> children;
  // The set of parents and classes where this node implements a previously
  // unimplemented method. (This is usually absent.)
  std::unique_ptr<OtherInterfaceImplementations>
      other_interface_implementations;
  // Whether the current Node's method is an interface method.
  bool is_interface;

  // Checks whther the current method's class, or any other implementation
  // class, can be cast to the given base type.
  bool overrides(const DexMethod* current, const DexType* base_type) const;
};

class Graph {
 public:
  const Node& get_node(const DexMethod* method) const;

  const ConcurrentMap<const DexMethod*, Node>& nodes() const { return m_nodes; }

  void add_edge(const DexMethod* overridden, const DexMethod* overriding);

  void add_edge(const DexMethod* overridden,
                bool overridden_is_interface,
                const DexMethod* overriding,
                bool overriding_is_interface);

  bool add_other_implementation_class(const DexMethod* overridden,
                                      const DexMethod* overriding,
                                      const DexClass* cls);

  void dump(std::ostream&) const;

 private:
  static Node empty_node;
  ConcurrentMap<const DexMethod*, Node> m_nodes;
};

} // namespace method_override_graph
