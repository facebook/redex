/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <unordered_map>

#include "DexClass.h"
#include "IRCode.h"
#include "MonotonicFixpointIterator.h"
#include "Resolver.h"

/*
 * Call graph representation that implements the standard graph interface
 * API for use with fixpoint iteration algorithms.
 */

namespace call_graph {

class Graph;

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
Graph single_callee_graph(const Scope&);

struct CallSite {
  const DexMethod* callee;
  IRList::iterator invoke;

  CallSite(const DexMethod* callee, IRList::iterator invoke)
      : callee(callee), invoke(invoke) {}
};

using CallSites = std::vector<CallSite>;

/*
 * This class determines how the call graph is built. The Graph ctor will start
 * from the roots and invoke get_callsites() on each returned method
 * recursively until the graph is fully mapped out. One can think of the
 * BuildStrategy as implicitly encoding the graph structure, with the Graph
 * constructor reifying it.
 */
class BuildStrategy {
 public:
  virtual ~BuildStrategy() {}

  virtual std::vector<const DexMethod*> get_roots() const = 0;

  virtual CallSites get_callsites(const DexMethod*) const = 0;
};

class Edge {
 public:
  Edge(const DexMethod* caller,
       const DexMethod* callee,
       const IRList::iterator& invoke_it);
  IRList::iterator invoke_iterator() const { return m_invoke_it; }
  const DexMethod* caller() const { return m_caller; }
  const DexMethod* callee() const { return m_callee; }

 private:
  const DexMethod* m_caller;
  const DexMethod* m_callee;
  IRList::iterator m_invoke_it;
};

using Edges = std::vector<std::shared_ptr<Edge>>;

class Node {
 public:
  /* implicit */
  Node(const DexMethod* m) : m_method(m) {}
  const DexMethod* method() const { return m_method; }
  bool operator==(const Node& that) const { return method() == that.method(); }
  const Edges& callers() const { return m_predecessors; }
  const Edges& callees() const { return m_successors; }

 private:
  const DexMethod* m_method;
  Edges m_predecessors;
  Edges m_successors;

  friend class Graph;
};

inline size_t hash_value(const Node& node) {
  return reinterpret_cast<size_t>(node.method());
}

} // namespace call_graph

namespace call_graph {

class Graph final {
 public:
  Graph(const BuildStrategy&);

  const Node& entry() const { return m_entry; }

  bool has_node(const DexMethod* m) const {
    return m_nodes.count(const_cast<DexMethod*>(m)) != 0;
  }

  const Node& node(const DexMethod* m) const {
    if (m == nullptr) {
      return m_entry;
    }
    return m_nodes.at(m);
  }

 private:
  Node& make_node(const DexMethod*);

  void add_edge(const DexMethod* caller,
                const DexMethod* callee,
                const IRList::iterator& invoke_it);

  Node m_entry = Node(nullptr);
  std::unordered_map<const DexMethod*, Node, boost::hash<Node>> m_nodes;
};

// A static-method-only API for use with the monotonic fixpoint iterator.
class GraphInterface {
 public:
  using Graph = call_graph::Graph;
  using NodeId = const DexMethod*;
  using EdgeId = std::shared_ptr<Edge>;

  static NodeId entry(const Graph& graph) { return graph.entry().method(); }
  static Edges predecessors(const Graph& graph, const NodeId& m) {
    return graph.node(m).callers();
  }
  static Edges successors(const Graph& graph, const NodeId& m) {
    return graph.node(m).callees();
  }
  static NodeId source(const Graph& graph, const EdgeId& e) {
    return graph.node(e->caller()).method();
  }
  static NodeId target(const Graph& graph, const EdgeId& e) {
    return graph.node(e->callee()).method();
  }
};

} // namespace call_graph
