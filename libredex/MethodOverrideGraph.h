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
 * Configuration for build_graph. Default is the cheap, miranda-free build
 * used by most consumers.
 */
struct GraphConfig {
  // When true, the builder synthesizes "miranda" nodes for every non-interface
  // class that directly declares an interface (via get_interfaces()) whose
  // methods are absent from the class's own vmethods. This includes:
  //
  //   1. Abstract classes with unimplemented interface obligations (the
  //      classic miranda case).
  //   2. Concrete classes that inherit the implementation from a superclass
  //      but declare the interface themselves. Because miranda synthesis
  //      uses IgnoreSuper policy (only considers interfaces declared on the
  //      class, not inherited via super), and checks the class's own
  //      vmethods (not inherited ones), a concrete class that inherits
  //      m() from super but declares `implements I` directly will get a
  //      miranda ref for I::m(). This is intentional: downstream consumers
  //      (e.g. MethodProfiles) need the DexMethodRef at the concrete
  //      class's type for profile-line resolution.
  //
  // Each miranda node represents the virtual dispatch slot at the class for
  // an interface method. Useful for consumers (e.g. VirtualMerging) that
  // need to group descendants by their nearest abstract dispatch root rather
  // than by the shared interface method.
  //
  // CAVEAT: Synthesizing a miranda node calls
  // DexMethod::make_method_downcast(class, name, proto), which mutates the
  // global DexMethod registry (creates a DexMethodRef if one doesn't exist).
  // Consumers should not share a non-miranda graph with code that requires
  // miranda semantics; see Graph::has_miranda().
  bool include_miranda{false};

  // Only meaningful when include_miranda is true. By default miranda synthesis
  // walks ALL classes known to g_redex (so refs exist at framework/other-store
  // types for consumers like MethodProfiles). When true, synthesis is
  // restricted to classes in the build `scope` plus external classes -- i.e.
  // the build_type_hierarchy(scope) node set -- so no DexMethodRef is
  // materialized for other stores' internal classes. This matches the locality
  // of legacy ClassScopes(scope); set it when the graph's mirandas are only
  // consumed for `scope`'s own hierarchy (e.g. VirtualScopes).
  bool miranda_within_scope{false};
};

/*
 * Slow-ish; users should build the graph once and cache it somewhere.
 */
std::unique_ptr<const Graph> build_graph(const Scope&);
std::unique_ptr<const Graph> build_graph(const Scope&, const GraphConfig&);

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
  bool is_interface{false};
  // Whether this node is a synthetic miranda dispatch slot. Only set when
  // the graph was built with GraphConfig::include_miranda=true. The node's
  // `method` is a DexMethodRef created via DexMethod::make_method_downcast;
  // there is no real DexMethod definition behind it.
  bool is_miranda{false};

  // Checks whether the current method's class, or any other implementation
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

  // True iff this graph was built with GraphConfig::include_miranda=true.
  // Consumers that depend on miranda semantics should assert this.
  bool has_miranda() const { return m_has_miranda; }

  // Internal: marks the graph as miranda-aware. Called by GraphBuilder.
  void set_has_miranda(bool v) { m_has_miranda = v; }

  // Internal: marks an existing node as a miranda slot. Called by
  // GraphBuilder during the miranda synthesis pass.
  void mark_miranda(const DexMethod* method);

 private:
  static const Node& get_empty_node() {
    static const Node empty_node;
    return empty_node;
  }
  ConcurrentMap<const DexMethod*, Node> m_nodes;
  bool m_has_miranda{false};
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

/*
 * Walk the superclass chain of `method->get_class()` and return the topmost
 * "class-level dispatch root" -- either a real vmethod with the same
 * (name, proto) or, if the graph was built with include_miranda=true, a
 * synthetic miranda slot at an abstract intermediate. Returns `method`
 * itself if no override target is found above it.
 *
 * For miranda-aware grouping (e.g. VirtualMerging), pass a graph built with
 * include_miranda=true; the miranda root distinguishes sibling subtrees that
 * would otherwise collapse into the same interface method.
 */
const DexMethod* find_class_dispatch_root(const Graph& graph,
                                          const DexMethod* method);

} // namespace method_override_graph
