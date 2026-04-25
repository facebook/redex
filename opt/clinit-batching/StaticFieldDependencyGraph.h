/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <vector>

#include "DeterministicContainers.h"
#include "DexClass.h"
#include "MethodOverrideGraph.h"

class StaticFieldDependencyGraphTest;

namespace clinit_batching {

/**
 * Represents the result of a topological sort operation.
 */
struct TopologicalSortResult {
  // Classes in dependency order (dependencies come before dependents)
  std::vector<DexClass*> ordered_classes;

  // Classes involved in cycles (excluded from ordered_classes)
  std::vector<DexClass*> cyclic_classes;

  // Number of classes involved in cycles
  size_t cyclic_classes_count{0};
};

/**
 * Tracks static field dependencies between classes based on their clinit
 * methods.
 *
 * A dependency exists from class A to class B if A's clinit:
 * - Reads a static field from B (SGET*)
 * - Invokes a static method on B (INVOKE_STATIC)
 * - Creates a new instance of B (NEW_INSTANCE)
 * - Calls a constructor or method that transitively reads static fields from B
 *
 * This graph is used to determine the correct initialization order for
 * batching clinits: dependencies must be initialized before dependents.
 */
class StaticFieldDependencyGraph {
 public:
  /**
   * Constructs an empty dependency graph.
   */
  StaticFieldDependencyGraph() = default;

  /**
   * Builds the dependency graph from the given candidate clinits.
   *
   * Analyzes each clinit's code to find SGET*, INVOKE_STATIC, and NEW_INSTANCE
   * instructions that reference other candidate classes.
   *
   * @param candidate_clinits Map of clinit methods to their containing classes
   */
  void build(const UnorderedMap<DexMethod*, DexClass*>& candidate_clinits,
             const method_override_graph::Graph* override_graph = nullptr,
             bool skip_benign = false);

  /**
   * Returns the classes that the given class depends on (its dependencies).
   */
  const UnorderedSet<DexClass*>& get_dependencies(DexClass* cls) const;

  /**
   * Returns the classes that depend on the given class (its dependents).
   */
  const UnorderedSet<DexClass*>& get_dependents(DexClass* cls) const;

  /**
   * Performs a topological sort of the classes in the graph.
   *
   * Classes are ordered so that dependencies come before dependents.
   * If cycles are detected, the cyclic classes are excluded from the
   * ordered result and reported separately.
   *
   * @return TopologicalSortResult containing ordered and cyclic classes
   */
  [[nodiscard]] TopologicalSortResult topological_sort() const;

  /**
   * Returns the total number of classes in the graph.
   */
  [[nodiscard]] size_t size() const { return m_classes.size(); }

  /**
   * Returns all classes in the graph.
   */
  const UnorderedSet<DexClass*>& get_all_classes() const { return m_classes; }

  /**
   * Checks if the given class is in the graph.
   */
  bool contains(DexClass* cls) const { return m_classes.count(cls) != 0; }

 private:
  friend class ::StaticFieldDependencyGraphTest;

  // DFS status for topological sort
  enum class DFSStatus { Unvisited, InProgress, Visited };

  /**
   * Adds a dependency: from_class depends on to_class.
   *
   * This means to_class must be initialized before from_class.
   *
   * @param from_class The class that depends on another
   * @param to_class The class being depended upon
   */
  void add_dependency(DexClass* from_class, DexClass* to_class);

  /**
   * DFS helper for topological sort with cycle detection.
   *
   * @param cls Current class being visited
   * @param status Map of DFS status for each class
   * @param sorted Output vector of sorted classes
   * @param in_cycle Set of classes detected in cycles
   * @return nullptr if no active cycle, or the cycle root if unwinding a cycle
   */
  DexClass* topological_sort_visit(DexClass* cls,
                                   UnorderedMap<DexClass*, DFSStatus>& status,
                                   std::vector<DexClass*>& sorted,
                                   UnorderedSet<DexClass*>& in_cycle) const;

  /**
   * Finds transitive dependencies from a method by analyzing its code for
   * instructions that reference candidate classes.
   *
   * Handles: SGET, SPUT (static field access), INVOKE_STATIC,
   * INVOKE_DIRECT, NEW_INSTANCE, and INVOKE_VIRTUAL/INVOKE_SUPER/
   * INVOKE_INTERFACE (when override_graph is provided).
   *
   * This is used to track dependencies through constructor and method calls.
   * For example, if clinit A calls new B(), and the B constructor reads
   * C.field, then A transitively depends on C.
   *
   * @param method The method to analyze
   * @param candidate_classes Set of classes that are candidates for batching
   * @param visited Set of already-visited methods (to avoid infinite loops)
   * @param found_deps Output set of dependency classes found
   */
  void find_transitive_dependencies(
      const DexMethod* method,
      const UnorderedSet<DexClass*>& candidate_classes,
      UnorderedSet<const DexMethod*>& visited,
      UnorderedSet<DexClass*>& found_deps,
      const method_override_graph::Graph* override_graph = nullptr,
      bool skip_benign = false) const;

  /**
   * Walks the superclass chain of cls, traversing each class's clinit for
   * transitive dependencies. Per JVMS 5.5, when a class is initialized, its
   * direct superclass must be initialized first (recursively).
   */
  void walk_clinit_chain(DexClass* cls,
                         const UnorderedSet<DexClass*>& candidate_classes,
                         UnorderedSet<const DexMethod*>& visited,
                         UnorderedSet<DexClass*>& found_deps,
                         const method_override_graph::Graph* override_graph,
                         bool skip_benign) const;

  // All classes in the graph
  UnorderedSet<DexClass*> m_classes;

  // Forward edges: class -> set of classes it depends on (predecessors)
  UnorderedMap<DexClass*, UnorderedSet<DexClass*>> m_dependencies;

  // Reverse edges: class -> set of classes that depend on it (successors)
  UnorderedMap<DexClass*, UnorderedSet<DexClass*>> m_dependents;

  // Returns an empty set for classes with no dependencies/dependents
  static const UnorderedSet<DexClass*>& empty_set() {
    static const UnorderedSet<DexClass*> s_empty_set;
    return s_empty_set;
  }
};

} // namespace clinit_batching
