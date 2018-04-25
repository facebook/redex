/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <unordered_map>

#include "DexClass.h"
#include "FixpointIterators.h"
#include "IRCode.h"
#include "Resolver.h"

/*
 * Call graph representation that implements the standard graph interface
 * API for use with fixpoint iteration algorithms.
 *
 *
 */

namespace call_graph {

class Edge {
 public:
  Edge(DexMethod* caller, DexMethod* callee, IRList::iterator invoke_it);
  IRList::iterator invoke_iterator() const { return m_invoke_it; }
  DexMethod* caller() const { return m_caller; }
  DexMethod* callee() const { return m_callee; }

  bool operator==(const Edge& that) const {
    return caller() == that.caller() && callee() == that.callee();
  }

 private:
  DexMethod* m_caller;
  DexMethod* m_callee;
  IRList::iterator m_invoke_it;
};

struct CompareEdges {
  bool operator()(const std::shared_ptr<Edge>& a,
                  const std::shared_ptr<Edge>& b) const {
    return *a == *b;
  }
};

struct HashEdges {
  size_t operator()(const std::shared_ptr<Edge>& e) const {
    return (size_t)(e->caller()) + ((size_t)e->callee());
  }
};

using Edges =
    std::unordered_set<std::shared_ptr<Edge>, HashEdges, CompareEdges>;

class Node {
 public:
  /* implicit */
  Node(DexMethod* m) : m_method(m) {}
  DexMethod* method() const { return m_method; }
  bool operator==(const Node& that) const { return method() == that.method(); }
  const Edges& callers() const { return m_predecessors; }
  const Edges& callees() const { return m_successors; }

 private:
  DexMethod* m_method;
  Edges m_predecessors;
  Edges m_successors;

  friend class Graph;
  friend class CompleteGraph;
};

/*
 * Currently, we only add edges in the graph when we know the exact callee
 * an invoke instruction refers to.  That means only invoke-static and
 * invoke-direct calls, as well as invoke-virtual calls that refer
 * unambiguously to a single method. This keeps the graph smallish and
 * easier to analyze.
 *
 * TODO: Once we have points-to information, we should expand the callgraph
 * to include invoke-virtuals that refer to sets of methods.
 */
class Graph {
 public:
  static Graph make(const Scope&, bool include_virtuals = false);

  const Node& entry() const { return m_entry; }

  const Node& node(const DexMethod* m) const {
    if (m == nullptr) {
      return m_entry;
    }
    return m_nodes.at(const_cast<DexMethod*>(m));
  }

  struct Cache {
    Cache(const Scope&, bool /* include_virtuals */);

    MethodRefCache m_resolved_refs;
    std::unordered_set<const DexMethod*> m_non_virtual;
  };

 protected:
  Graph() {}

  // Factor out the logic to populate the graph and select the roots
  void populate_graph(const Scope&, bool /* include_virtuals */, Cache&);
  void compute_roots(Cache&);

  // helper functions
  bool is_definitely_virtual(const DexMethod*,
                             const std::unordered_set<const DexMethod*>&) const;

  Node& make_node(DexMethod*);

  void add_edge(DexMethod* caller,
                DexMethod* callee,
                IRList::iterator invoke_it);

  Node m_entry = Node(nullptr);
  std::unordered_map<DexMethod*, Node> m_nodes;
};

/*
 * We add all the edges from callers to callees, even if the callee is
 * unresolved
 *
 * TODO: Once the Points-to analysis is available, use it
 */
class CompleteGraph : public Graph {
 public:
  static CompleteGraph make(const Scope&, bool include_virtuals = false);

 protected:
  explicit CompleteGraph() : Graph() {}

  void populate_graph(const Scope&, bool /* include_virtuals */, Cache&);
  void compute_roots(Cache&);
};

// A static-method-only API for use with the monotonic fixpoint iterator.
class GraphInterface {
 public:
  using Graph = call_graph::Graph;
  using NodeId = DexMethod*;
  using EdgeId = std::shared_ptr<Edge>;

  static const NodeId entry(const Graph& graph) {
    return graph.entry().method();
  }
  static Edges predecessors(const Graph& graph, const NodeId& m) {
    return graph.node(m).callers();
  }
  static Edges successors(const Graph& graph, const NodeId& m) {
    return graph.node(m).callees();
  }
  static const NodeId source(const Graph& graph, const EdgeId& e) {
    return graph.node(e->caller()).method();
  }
  static const NodeId target(const Graph& graph, const EdgeId& e) {
    return graph.node(e->callee()).method();
  }
};

} // namespace call_graph

namespace std {

template <>
struct hash<call_graph::Node> {
  size_t operator()(const call_graph::Node& node) const {
    return reinterpret_cast<size_t>(node.method());
  }
};

} // namespace std
