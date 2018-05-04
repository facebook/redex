/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <boost/functional/hash.hpp>
#include <unordered_map>

#include "DexClass.h"
#include "FixpointIterators.h"
#include "IRCode.h"
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
  DexMethod* callee;
  IRList::iterator invoke;

  CallSite(DexMethod* callee, IRList::iterator invoke)
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

  virtual std::vector<DexMethod*> get_roots() const = 0;

  virtual CallSites get_callsites(const DexMethod*) const = 0;
};

class Edge {
 public:
  Edge(DexMethod* caller, DexMethod* callee, IRList::iterator invoke_it);
  IRList::iterator invoke_iterator() const { return m_invoke_it; }
  DexMethod* caller() const { return m_caller; }
  DexMethod* callee() const { return m_callee; }

 private:
  DexMethod* m_caller;
  DexMethod* m_callee;
  IRList::iterator m_invoke_it;
};

using Edges = std::vector<std::shared_ptr<Edge>>;

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

  const Node& node(const DexMethod* m) const {
    if (m == nullptr) {
      return m_entry;
    }
    return m_nodes.at(const_cast<DexMethod*>(m));
  }

 private:
  Node& make_node(DexMethod*);

  void add_edge(DexMethod* caller,
                DexMethod* callee,
                IRList::iterator invoke_it);

  Node m_entry = Node(nullptr);
  std::unordered_map<DexMethod*, Node, boost::hash<Node>> m_nodes;
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
