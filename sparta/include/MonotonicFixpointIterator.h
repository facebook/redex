/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "AbstractDomain.h"
#include "FixpointIterator.h"
#include "WeakTopologicalOrdering.h"

namespace sparta {

/*
 * This data structure contains the current state of the fixpoint iteration,
 * which is provided to the user when an extrapolation step is executed, so as
 * to decide when to perform widening. For each SCC head in the weak topological
 * ordering of a control-flow graph, the context records the number of times the
 * node has been analyzed overall, as well as the number of times it has been
 * analyzed in the current local stabilization loop (please see Bourdoncle's
 * paper for more details on the recursive iteration strategy).
 */
template <typename NodeId, typename Domain, typename NodeHash>
class MonotonicFixpointIteratorContext final {
 public:
  MonotonicFixpointIteratorContext() = delete;
  MonotonicFixpointIteratorContext(const MonotonicFixpointIteratorContext&) =
      delete;

  uint32_t get_local_iterations_for(const NodeId& node) const {
    auto it = m_local_iterations.find(node);
    if (it == m_local_iterations.end()) {
      return 0;
    }
    return it->second;
  }

  uint32_t get_global_iterations_for(const NodeId& node) const {
    auto it = m_global_iterations.find(node);
    if (it == m_global_iterations.end()) {
      return 0;
    }
    return it->second;
  }

 private:
  explicit MonotonicFixpointIteratorContext(const Domain& init)
      : m_init(init) {}

  const Domain& get_initial_value() const { return m_init; }

  void increase_iteration_count(
      const NodeId& node,
      std::unordered_map<NodeId, uint32_t, NodeHash>* table) {
    auto insertion = table->insert({node, 1});
    if (insertion.second == false) {
      ++insertion.first->second;
    }
  }

  void increase_iteration_count_for(const NodeId& node) {
    increase_iteration_count(node, &m_local_iterations);
    increase_iteration_count(node, &m_global_iterations);
  }

  void reset_local_iteration_count_for(const NodeId& node) {
    m_local_iterations.erase(node);
  }

  const Domain& m_init;
  std::unordered_map<NodeId, uint32_t, NodeHash> m_global_iterations;
  std::unordered_map<NodeId, uint32_t, NodeHash> m_local_iterations;

  template <typename T1, typename T2, typename T3>
  friend class MonotonicFixpointIterator;
};

/*
 * This is the implementation of a monotonically increasing chaotic fixpoint
 * iteration sequence with widening over a control-flow graph (CFG) using the
 * recursive iteration strategy induced by a weak topological ordering of the
 * nodes in the control-flow graph. The recursive iteration strategy is
 * described in Bourdoncle's paper on weak topological orderings:
 *
 *   F. Bourdoncle. Efficient chaotic iteration strategies with widenings.
 *   In Formal Methods in Programming and Their Applications, pp 128-141.
 */
template <typename GraphInterface,
          typename Domain,
          typename NodeHash = std::hash<typename GraphInterface::NodeId>>
class MonotonicFixpointIterator
    : public FixpointIterator<GraphInterface, Domain> {
 public:
  using Graph = typename GraphInterface::Graph;
  using NodeId = typename GraphInterface::NodeId;
  using EdgeId = typename GraphInterface::EdgeId;
  using Context = MonotonicFixpointIteratorContext<NodeId, Domain, NodeHash>;

  /*
   * When the number of nodes in the CFG is known, it's better to provide it to
   * the constructor, so as to prevent unnecessary resizing of the underlying
   * hashtables during the iteration.
   */
  MonotonicFixpointIterator(const Graph& graph, size_t cfg_size_hint = 4)
      : m_graph(graph),
        m_wto(GraphInterface::entry(graph),
              [=, &graph](const NodeId& x) {
                const auto& succ_edges = GraphInterface::successors(graph, x);
                std::vector<NodeId> succ_nodes;
                std::transform(succ_edges.begin(),
                               succ_edges.end(),
                               std::back_inserter(succ_nodes),
                               std::bind(&GraphInterface::target,
                                         std::ref(graph),
                                         std::placeholders::_1));
                return succ_nodes;
              }),
        m_entry_states(cfg_size_hint),
        m_exit_states(cfg_size_hint) {}

  /*
   * This method is invoked on the head of an SCC at each iteration, whenever
   * the newly computed entry state is not subsumed by the current one. In
   * order to converge, the widening operator must be applied infinitely many
   * often. However, the order and frequency at which it is performed may have a
   * very significant impact on the precision of the final result. This method
   * gives the user a way to parameterize the application of the widening
   * operator. A default widening strategy is provided, which applies the join
   * at the first iteration and then the widening at all subsequent iterations
   * until the limit is reached.
   */
  virtual void extrapolate(const Context& context,
                           const NodeId& node,
                           Domain* current_state,
                           const Domain& new_state) const {
    if (context.get_local_iterations_for(node) == 0) {
      current_state->join_with(new_state);
    } else {
      current_state->widen_with(new_state);
    }
  }

  /*
   * Executes the fixpoint iterator given an abstract value describing the
   * initial program configuration. This method can be invoked multiple times
   * with different values in order to analyze the program under different
   * initial conditions.
   */
  void run(const Domain& init) {
    clear();
    Context context(init);
    for (const WtoComponent<NodeId>& component : m_wto) {
      analyze_component(&context, component);
    }
  }

  /*
   * Returns the invariant computed by the fixpoint iterator at a node entry.
   */
  Domain get_entry_state_at(const NodeId& node) const {
    auto it = m_entry_states.find(node);
    return (it == m_entry_states.end()) ? Domain::bottom() : it->second;
  }

  /*
   * Returns the invariant computed by the fixpoint iterator at a node exit.
   */
  Domain get_exit_state_at(const NodeId& node) const {
    auto it = m_exit_states.find(node);
    // It's impossible to get rid of this condition by initializing all exit
    // states to _|_ prior to starting the fixpoint iteration. The reason is
    // that we only have a partial view of the control-flow graph, i.e., all
    // nodes that are reachable from the root. We may have control-flow graphs
    // with unreachable nodes pointing to reachable ones, as follows:
    //
    //               root
    //           U    |
    //           |    V
    //           +--> A
    //
    // When computing the entry state of A, we perform the join of the exit
    // states of all its predecessors, which include U. Since U is invisible to
    // the fixpoint iterator, there is no way to initialize its exit state.
    return (it == m_exit_states.end()) ? Domain::bottom() : it->second;
  }

 private:
  void clear() {
    m_entry_states.clear();
    m_exit_states.clear();
  }

  void compute_entry_state(Context* context,
                           const NodeId& node,
                           Domain* placeholder) {
    placeholder->set_to_bottom();
    if (node == GraphInterface::entry(m_graph)) {
      placeholder->join_with(context->get_initial_value());
    }
    for (EdgeId edge : GraphInterface::predecessors(m_graph, node)) {
      placeholder->join_with(this->analyze_edge(
          edge, get_exit_state_at(GraphInterface::source(m_graph, edge))));
    }
  }

  void analyze_component(Context* context,
                         const WtoComponent<NodeId>& component) {
    if (component.is_vertex()) {
      analyze_vertex(context, component.head_node());
    } else {
      analyze_scc(context, component);
    }
  }

  void analyze_vertex(Context* context, const NodeId& node) {
    Domain& entry_state = m_entry_states[node];
    // We should be careful not to access m_exit_states[node] before computing
    // the entry state, as this may silently initialize it with an unwanted
    // value (i.e., the default-constructed value of Domain). This can in turn
    // lead to inaccurate or even incorrect results when the node possesses a
    // self-loop. Initializing all exit states prior to starting the fixpoint
    // iteration is not a viable option, since the control-flow graph may
    // contain unreachable nodes pointing to reachable ones (see the
    // documentation of `get_exit_state_at`).
    compute_entry_state(context, node, &entry_state);
    Domain& exit_state = m_exit_states[node];
    exit_state = entry_state;
    this->analyze_node(node, &exit_state);
  }

  void analyze_scc(Context* context, const WtoComponent<NodeId>& scc) {
    NodeId head = scc.head_node();
    bool iterate = true;
    for (context->reset_local_iteration_count_for(head); iterate;
         context->increase_iteration_count_for(head)) {
      analyze_vertex(context, head);
      for (const auto& component : scc) {
        analyze_component(context, component);
      }
      // The current state of the iteration is represented by a pointer to the
      // slot associated with the head node in the hash table of entry states.
      // The state is updated in place within the hash table via side effects,
      // which avoids costly copies and allocations.
      Domain* current_state = &m_entry_states[head];
      Domain new_state;
      compute_entry_state(context, head, &new_state);
      if (new_state.leq(*current_state)) {
        // At this point we know that the monotonic iteration sequence has
        // converged and current_state is a post-fixpoint. However, since all
        // the node and edge transformers are monotonic, new_state is also a
        // post-fixpoint (this is essentially the argument for performing a
        // decreasing iteration sequence with narrowing after a post-fixpoint
        // has been reached using an increasing iteration sequence with
        // widening). Since new_state may be more precise than current_state,
        // it's better to use it as the final result of the iteration sequence.
        *current_state = std::move(new_state);
        iterate = false;
      } else {
        extrapolate(*context, head, current_state, new_state);
      }
    }
  }

  const Graph& m_graph;
  WeakTopologicalOrdering<NodeId, NodeHash> m_wto;
  std::unordered_map<NodeId, Domain, NodeHash> m_entry_states;
  std::unordered_map<NodeId, Domain, NodeHash> m_exit_states;
};

/*
 * This combinator takes the specification of a CFG and produces an interface to
 * the reverse CFG, where the direction of edges has been flipped. The original
 * CFG must expose an exit node, which becomes the entry node of the reverse
 * CFG. The purpose of this transformation is to perform a backwards analysis
 * (e.g., live variable analysis). In the theory of Abstract Interpretation,
 * performing a backwards analysis simply amounts to performing a forwards
 * analysis on the reverse CFG.
 */
template <typename GraphInterface>
class BackwardsFixpointIterationAdaptor {
 public:
  using Graph = typename GraphInterface::Graph;
  using NodeId = typename GraphInterface::NodeId;
  using EdgeId = typename GraphInterface::EdgeId;

  static NodeId entry(const Graph& graph) {
    static_assert(
        std::is_same<decltype(GraphInterface::exit(std::declval<Graph>())),
                     NodeId>::value,
        "No implementation of exit()");
    return GraphInterface::exit(graph);
  }
  static NodeId exit(const Graph& graph) {
    return GraphInterface::entry(graph);
  }
  static std::vector<EdgeId> predecessors(const Graph& graph,
                                          const NodeId& node) {
    return GraphInterface::successors(graph, node);
  }
  static std::vector<EdgeId> successors(const Graph& graph,
                                        const NodeId& node) {
    return GraphInterface::predecessors(graph, node);
  }
  static NodeId source(const Graph& graph, const EdgeId& edge) {
    return GraphInterface::target(graph, edge);
  }
  static NodeId target(const Graph& graph, const EdgeId& edge) {
    return GraphInterface::source(graph, edge);
  }
};

} // namespace sparta
