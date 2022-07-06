/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>

#include "DexClass.h"
#include "IRCode.h"
#include "MonotonicFixpointIterator.h"
#include "Resolver.h"

namespace method_override_graph {
class Graph;
} // namespace method_override_graph

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
Graph single_callee_graph(
    const method_override_graph::Graph& method_override_graph, const Scope&);

Graph multiple_callee_graph(
    const method_override_graph::Graph& method_override_graph,
    const Scope&,
    uint32_t big_override_threshold);

Graph complete_call_graph(
    const method_override_graph::Graph& method_override_graph, const Scope&);

struct CallSite {
  const DexMethod* callee;
  IRInstruction* invoke_insn;

  CallSite(const DexMethod* callee, IRInstruction* invoke_insn)
      : callee(callee), invoke_insn(invoke_insn) {}
};

using CallSites = std::vector<CallSite>;
using MethodSet = std::unordered_set<const DexMethod*>;

struct RootAndDynamic {
  std::vector<const DexMethod*> roots;
  std::unordered_set<const DexMethod*> dynamic_methods;
};

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

  virtual RootAndDynamic get_roots() const = 0;

  virtual CallSites get_callsites(const DexMethod*) const = 0;
};

class Edge;
using EdgeId = std::shared_ptr<Edge>;
using Edges = std::vector<std::shared_ptr<Edge>>;

class Node {
  enum NodeType {
    GHOST_ENTRY,
    GHOST_EXIT,
    REAL_METHOD,
  };

 public:
  explicit Node(const DexMethod* m) : m_method(m), m_type(REAL_METHOD) {}
  explicit Node(NodeType type) : m_method(nullptr), m_type(type) {}

  const DexMethod* method() const { return m_method; }
  bool operator==(const Node& that) const { return method() == that.method(); }
  const Edges& callers() const { return m_predecessors; }
  const Edges& callees() const { return m_successors; }

  bool is_entry() { return m_type == GHOST_ENTRY; }
  bool is_exit() { return m_type == GHOST_EXIT; }

 private:
  const DexMethod* m_method;
  Edges m_predecessors;
  Edges m_successors;
  NodeType m_type;

  friend class Graph;
};

using NodeId = Node*;

class Edge {
 public:
  Edge(NodeId caller, NodeId callee, IRInstruction* invoke_insn);
  IRInstruction* invoke_insn() const { return m_invoke_insn; }
  NodeId caller() const { return m_caller; }
  NodeId callee() const { return m_callee; }

 private:
  NodeId m_caller;
  NodeId m_callee;
  IRInstruction* m_invoke_insn;
};

class Graph final {
 public:
  explicit Graph(const BuildStrategy&);

  NodeId entry() const { return m_entry.get(); }
  NodeId exit() const { return m_exit.get(); }

  bool has_node(const DexMethod* m) const {
    return m_nodes.count(const_cast<DexMethod*>(m)) != 0;
  }

  NodeId node(const DexMethod* m) const {
    if (m == nullptr) {
      return this->entry();
    }
    return m_nodes.at(m).get();
  }

  const std::unordered_map<const IRInstruction*,
                           std::unordered_set<const DexMethod*>>&
  get_insn_to_callee() const {
    return m_insn_to_callee;
  }

  const std::unordered_set<const DexMethod*>& get_dynamic_methods() const {
    return m_dynamic_methods;
  }

 private:
  NodeId make_node(const DexMethod*);

  void add_edge(const NodeId& caller,
                const NodeId& callee,
                IRInstruction* invoke_insn);

  std::shared_ptr<Node> m_entry;
  std::shared_ptr<Node> m_exit;
  std::unordered_map<const DexMethod*, std::shared_ptr<Node>> m_nodes;
  std::unordered_map<const IRInstruction*, std::unordered_set<const DexMethod*>>
      m_insn_to_callee;
  // Methods that might have unknown inputs/outputs that we need special handle.
  // Like external methods with internal overrides, they might have external
  // implementation that we don't know about. Or methods that might have
  // dynamically added implementations, created via Proxy.newProxyInstance.
  // Or methods with native implementation.
  // We are only collecting those for multiple callee callgraph because we
  // need to avoid propagating method return values for those true virtual
  // methods.
  std::unordered_set<const DexMethod*> m_dynamic_methods;
};

class SingleCalleeStrategy : public BuildStrategy {
 public:
  explicit SingleCalleeStrategy(const method_override_graph::Graph&,
                                const Scope& scope);
  CallSites get_callsites(const DexMethod* method) const override;
  RootAndDynamic get_roots() const override;

 protected:
  bool is_definitely_virtual(DexMethod* method) const;
  virtual DexMethod* resolve_callee(const DexMethod* caller,
                                    IRInstruction* invoke) const;

  const Scope& m_scope;
  std::unordered_set<DexMethod*> m_non_virtual;
  mutable MethodRefCache m_resolved_refs;
};

class MultipleCalleeBaseStrategy : public SingleCalleeStrategy {
 public:
  explicit MultipleCalleeBaseStrategy(const method_override_graph::Graph&,
                                      const Scope& scope);

  CallSites get_callsites(const DexMethod* method) const override = 0;
  RootAndDynamic get_roots() const override;

 protected:
  virtual std::vector<const DexMethod*> get_additional_roots(
      const MethodSet& /* existing_roots */) const {
    // No additional roots by default.
    return std::vector<const DexMethod*>();
  }

  const method_override_graph::Graph& m_method_override_graph;
};

class CompleteCallGraphStrategy : public MultipleCalleeBaseStrategy {
 public:
  explicit CompleteCallGraphStrategy(const method_override_graph::Graph&,
                                     const Scope& scope);
  CallSites get_callsites(const DexMethod* method) const override;
  RootAndDynamic get_roots() const override;
};

class MultipleCalleeStrategy : public MultipleCalleeBaseStrategy {
 public:
  explicit MultipleCalleeStrategy(const method_override_graph::Graph&,
                                  const Scope& scope,
                                  uint32_t big_override_threshold);
  CallSites get_callsites(const DexMethod* method) const override;

 protected:
  std::vector<const DexMethod*> get_additional_roots(
      const MethodSet& existing_roots) const override;
  MethodSet m_big_override;
};

// A static-method-only API for use with the monotonic fixpoint iterator.
class GraphInterface {
 public:
  using Graph = call_graph::Graph;
  using NodeId = Node*;
  using EdgeId = std::shared_ptr<Edge>;

  static NodeId entry(const Graph& graph) { return graph.entry(); }
  static NodeId exit(const Graph& graph) { return graph.exit(); }
  static Edges predecessors(const Graph& graph, const NodeId& m) {
    return m->callers();
  }
  static Edges successors(const Graph& graph, const NodeId& m) {
    return m->callees();
  }
  static NodeId source(const Graph& graph, const EdgeId& e) {
    return e->caller();
  }
  static NodeId target(const Graph& graph, const EdgeId& e) {
    return e->callee();
  }
};

MethodSet resolve_callees_in_graph(const Graph& graph,
                                   const DexMethod* method,
                                   const IRInstruction* insn);

MethodSet resolve_callees_in_graph(const Graph& graph,
                                   const IRInstruction* insn);

bool method_is_dynamic(const Graph& graph, const DexMethod* method);

struct CallgraphStats {
  uint32_t num_nodes;
  uint32_t num_edges;
  uint32_t num_callsites;
  CallgraphStats(uint32_t num_nodes, uint32_t num_edges, uint32_t num_callsites)
      : num_nodes(num_nodes),
        num_edges(num_edges),
        num_callsites(num_callsites) {}
};

CallgraphStats get_num_nodes_edges(const Graph& graph);

} // namespace call_graph
