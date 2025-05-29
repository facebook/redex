/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConcurrentContainers.h"
#include "DeterministicContainers.h"
#include "DexClass.h"
#include "DexStore.h"

// The definition of TypeSet is defined differently in ClassHierarchy, so we
// need to manually define ClassHierarchy here.
using ClassHierarchy =
    UnorderedMap<const DexType*, std::set<const DexType*, dextypes_comparator>>;

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
 */
UnorderedBag<const DexMethod*> get_overriding_methods(
    const Graph& graph,
    const DexMethod* method,
    bool include_interfaces = false,
    const DexType* base_type = nullptr);

/*
 * Returns all the methods that are overridden by :method. The set does *not*
 * include the :method itself.
 */
UnorderedBag<const DexMethod*> get_overridden_methods(
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
InsertOnlyConcurrentSet<DexMethod*> get_non_true_virtuals(const Graph& graph,
                                                          const Scope& scope);

/*
 * When a class method implements interface methods only in a subclass of the
 * method's declaring class, then we need to track additional information.
 */
struct OtherInterfaceImplementations {
  // The set of immediately implemented interface methods.
  UnorderedSet<const DexMethod*> parents;
  // The set of the classes for which the current method implements those
  // interface methods for the first time.
  UnorderedBag<const DexClass*> classes;
};

/*
 * The `children` edges point to the overriders / implementors of the current
 * Node's method.
 */
struct Node {
  const DexMethod* method{nullptr};
  // The set of immediately overridden / implemented methods.
  UnorderedBag<Node*> parents;
  // The set of immediately overriding / implementing methods.
  UnorderedBag<Node*> children;
  // The set of parents and classes where this node implements a previously
  // unimplemented method. (This is usually absent.)
  std::unique_ptr<OtherInterfaceImplementations>
      other_interface_implementations;
  // Whether the current Node's method is an interface method.
  bool is_interface;

  // Checks whther the current method's class, or any other implementation
  // class, can be cast to the given base type.
  bool overrides(const DexMethod* current, const DexType* base_type) const;

  void gather_connected_methods(UnorderedSet<const DexMethod*>* visited) const;
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

bool all_overriding_methods(const Graph& graph,
                            const DexMethod* method,
                            const std::function<bool(const DexMethod*)>& f,
                            bool include_interfaces = false,
                            const DexType* base_type = nullptr);

bool any_overriding_methods(
    const Graph& graph,
    const DexMethod* method,
    const std::function<bool(const DexMethod*)>& f = [](auto*) { return true; },
    bool include_interfaces = false,
    const DexType* base_type = nullptr);

bool all_overridden_methods(const Graph& graph,
                            const DexMethod* method,
                            const std::function<bool(const DexMethod*)>& f,
                            bool include_interfaces);

bool any_overridden_methods(
    const Graph& graph,
    const DexMethod* method,
    const std::function<bool(const DexMethod*)>& f = [](auto*) { return true; },
    bool include_interfaces = false);

UnorderedSet<DexClass*> get_classes_with_overridden_finalize(
    const Graph& method_override_graph, const ClassHierarchy& class_hierarchy);

} // namespace method_override_graph
