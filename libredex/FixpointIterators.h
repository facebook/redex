/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "AbstractDomain.h"
#include "Debug.h"
#include "WeakTopologicalOrdering.h"

/*
 * This data structure contains the current state of the fixpoint iteration,
 * which is provided to the user when an extrapolation step is executed, so as
 * to decide when to perform widening. For each SCC head in the weak topological
 * ordering of a control-flow graph, the context records the number of times the
 * node has been analyzed overall, as well as the number of times it has been
 * analyzed in the current local stabilization loop (please see Bourdoncle's
 * paper for more details on the recursive iteration strategy).
 */
template <typename NodeId, typename Domain>
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

  void increase_iteration_count(const NodeId& node,
                                std::unordered_map<NodeId, uint32_t>* table) {
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
  std::unordered_map<NodeId, uint32_t> m_global_iterations;
  std::unordered_map<NodeId, uint32_t> m_local_iterations;

  template <typename T1, typename T2>
  friend class MonotonicFixpointIterator;
};

/*
 * This is the implementation of a monotonically increasing chaotic fixpoint
 * iteration sequence with widening over a control-flow graph using the
 * recursive iteration strategy induced by a weak topological ordering of the
 * nodes in the control-flow graph. A detailed exposition of chaotic fixpoint
 * iteration and its use in Abstract Interpretation can be found in the
 * following paper:
 *
 *  Patrick Cousot & Radhia Cousot. Abstract interpretation and application to
 *  logic programs. Journal of Logic Programming, 13(2—3):103—179, 1992.
 *
 * The recursive iteration strategy is described in Bourdoncle's paper on weak
 * topological orderings.
 *
 * The fixpoint iterator is thread safe.
 */
template <typename NodeId, typename Domain>
class MonotonicFixpointIterator {
 public:
  using Context = MonotonicFixpointIteratorContext<NodeId, Domain>;

  virtual ~MonotonicFixpointIterator() {
    static_assert(std::is_base_of<AbstractDomain<Domain>, Domain>::value,
                  "Domain does not inherit from AbstractDomain");
  }

  /*
   * The control-flow graph is specified by its root node together with the
   * successors and predecessors functions. When the number of nodes in the CFG
   * is known, it's better to provide it to the constructor, so as to prevent
   * unnecessary resizing of the underlying hashtables during the iteration.
   */
  MonotonicFixpointIterator(
      NodeId root,
      std::function<std::vector<NodeId>(const NodeId&)> successors,
      std::function<std::vector<NodeId>(const NodeId&)> predecessors,
      size_t cfg_size_hint = 4)
      : m_root(root),
        m_predecessors(predecessors),
        m_wto(root, successors),
        m_entry_states(cfg_size_hint),
        m_exit_states(cfg_size_hint) {}

  /*
   * This method implements the semantic transformer for each node in the
   * control-flow graph. For better performance, the transformer operates by
   * modifying the current state via side effects (hence the pointer to an
   * abstract value). The method is invoked with an abstract value describing
   * the state of the program upon entering the node. When the method returns,
   * the abstract value 'current_state' should contain the state of the program
   * after the node has been processed. If a node represents a basic block, the
   * same abstract value can be used in sequence to analyze all instructions in
   * the block, thus avoiding costly copies between instructions.
   *
   * Node transformers are required to be monotonic.
   *
   */
  virtual void analyze_node(const NodeId& node,
                            Domain* current_state) const = 0;

  /*
   * Edges in the control-flow graph may be associated with different behaviors
   * that have distinct semantics (conditional branch, exception, etc.). This
   * method describes the effect of traversing an outgoing edge on the state of
   * the program, when the source node is exited and control is transferred over
   * to the target node.
   *
   * Edge transformers are required to be monotonic.
   *
   */
  virtual Domain analyze_edge(const NodeId& source,
                              const NodeId& target,
                              const Domain& exit_state_at_source) const = 0;

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
    std::lock_guard<std::recursive_mutex> guard(m_lock);
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
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    auto it = m_entry_states.find(node);
    return (it == m_entry_states.end()) ? Domain::bottom() : it->second;
  }

  /*
   * Returns the invariant computed by the fixpoint iterator at a node exit.
   */
  Domain get_exit_state_at(const NodeId& node) const {
    std::lock_guard<std::recursive_mutex> guard(m_lock);
    auto it = m_exit_states.find(node);
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
    if (node == m_root) {
      placeholder->join_with(context->get_initial_value());
    }
    for (NodeId pred : m_predecessors(node)) {
      placeholder->join_with(analyze_edge(pred, node, get_exit_state_at(pred)));
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
    // self-loop.
    compute_entry_state(context, node, &entry_state);
    Domain& exit_state = m_exit_states[node];
    exit_state = entry_state;
    analyze_node(node, &exit_state);
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

  mutable std::recursive_mutex m_lock;
  NodeId m_root;
  std::function<std::vector<NodeId>(const NodeId&)> m_predecessors;
  WeakTopologicalOrdering<NodeId> m_wto;
  std::unordered_map<NodeId, Domain> m_entry_states;
  std::unordered_map<NodeId, Domain> m_exit_states;
};
