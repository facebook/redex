/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <sparta/AbstractDomain.h>
#include <sparta/FixpointIterator.h>
#include <sparta/WeakPartialOrdering.h>
#include <sparta/WeakTopologicalOrdering.h>
#include <sparta/WorkQueue.h>

namespace sparta {

namespace fp_impl {

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

  explicit MonotonicFixpointIteratorContext(const Domain& init)
      : m_init(init) {}

  explicit MonotonicFixpointIteratorContext(
      const Domain& init, const std::unordered_set<NodeId>& nodes)
      : m_init(init) {
    // Pre-populate hash table for all the nodes.
    for (auto& node : nodes) {
      m_global_iterations[node] = 0;
      m_local_iterations[node] = 0;
    }
  }

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
    m_local_iterations[node] = 0;
  }

 private:
  const Domain& m_init;
  std::unordered_map<NodeId, uint32_t, NodeHash> m_global_iterations;
  std::unordered_map<NodeId, uint32_t, NodeHash> m_local_iterations;
};

/*
 * Shared by MonotonicFixpointIterator and ParallelMonotonicFixpointIterator,
 * do not use directly.
 */
template <typename GraphInterface,
          typename Domain,
          typename NodeHash = std::hash<typename GraphInterface::NodeId>>
class MonotonicFixpointIteratorBase
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
  explicit MonotonicFixpointIteratorBase(const Graph& graph,
                                         size_t cfg_size_hint = 4)
      : m_graph(graph),
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
   * Returns the invariant computed by the fixpoint iterator at a node entry.
   */
  const Domain& get_entry_state_at(const NodeId& node) const {
    auto it = m_entry_states.find(node);
    return (it == m_entry_states.end()) ? m_bottom_state : it->second;
  }

  /*
   * Returns the invariant computed by the fixpoint iterator at a node exit.
   */
  const Domain& get_exit_state_at(const NodeId& node) const {
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
    return (it == m_exit_states.end()) ? m_bottom_state : it->second;
  }

  void clear() {
    m_entry_states.clear();
    m_exit_states.clear();
  }

  void compute_entry_state(Context* context,
                           const NodeId& node,
                           Domain* entry_state) {
    if (node == GraphInterface::entry(m_graph)) {
      entry_state->join_with(context->get_initial_value());
    }
    for (EdgeId edge : GraphInterface::predecessors(m_graph, node)) {
      entry_state->join_with(this->analyze_edge(
          edge, get_exit_state_at(GraphInterface::source(m_graph, edge))));
    }
  }

  void analyze_vertex(Context* context, const NodeId& node) {
    // Retrieve the entry state. If it does not exist, set it to bottom.
    Domain& entry_state =
        m_entry_states.emplace(node, Domain::bottom()).first->second;
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

  const Graph& m_graph;
  const Domain m_bottom_state = Domain::bottom();
  std::unordered_map<NodeId, Domain, NodeHash> m_entry_states;
  std::unordered_map<NodeId, Domain, NodeHash> m_exit_states;
};

template <typename GraphInterface, typename NodeHash>
class SuccessorNodeListBuilder {
  using Graph = typename GraphInterface::Graph;
  using NodeId = typename GraphInterface::NodeId;

 public:
  explicit SuccessorNodeListBuilder(const Graph& graph) : m_graph(graph) {}

  std::vector<NodeId> operator()(const NodeId& x) {
    const auto& succ_edges = GraphInterface::successors(m_graph, x);

    std::vector<NodeId> succ_nodes;
    std::transform(succ_edges.begin(),
                   succ_edges.end(),
                   std::inserter(succ_nodes, succ_nodes.end()),
                   std::bind(&GraphInterface::target,
                             std::ref(m_graph),
                             std::placeholders::_1));

    // Deduplicate elements
    std::unordered_set<NodeId, NodeHash> succ_nodes_dedup_set{
        succ_nodes.begin(), succ_nodes.end()};

    std::vector<NodeId> ret;
    std::copy_if(std::make_move_iterator(succ_nodes.begin()),
                 std::make_move_iterator(succ_nodes.end()),
                 std::back_inserter(ret),
                 [&succ_nodes_dedup_set](const NodeId& id) {
                   return succ_nodes_dedup_set.find(id) !=
                          succ_nodes_dedup_set.end();
                 });
    return ret;
  }

 private:
  const Graph& m_graph;
};

} // namespace fp_impl

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
class WTOMonotonicFixpointIterator
    : public fp_impl::
          MonotonicFixpointIteratorBase<GraphInterface, Domain, NodeHash> {
 public:
  using Graph = typename GraphInterface::Graph;
  using NodeId = typename GraphInterface::NodeId;
  using EdgeId = typename GraphInterface::EdgeId;
  using Context =
      fp_impl::MonotonicFixpointIteratorContext<NodeId, Domain, NodeHash>;

  explicit WTOMonotonicFixpointIterator(const Graph& graph,
                                        size_t cfg_size_hint = 4)
      : fp_impl::MonotonicFixpointIteratorBase<GraphInterface,
                                               Domain,
                                               NodeHash>(graph, cfg_size_hint),
        m_wto(GraphInterface::entry(graph), [=, &graph](const NodeId& x) {
          const auto& succ_edges = GraphInterface::successors(graph, x);
          std::vector<NodeId> succ_nodes;
          std::transform(succ_edges.begin(),
                         succ_edges.end(),
                         std::back_inserter(succ_nodes),
                         std::bind(&GraphInterface::target,
                                   std::ref(graph),
                                   std::placeholders::_1));
          return succ_nodes;
        }) {}

  /*
   * Executes the fixpoint iterator given an abstract value describing the
   * initial program configuration. This method can be invoked multiple times
   * with different values in order to analyze the program under different
   * initial conditions.
   */
  void run(const Domain& init) {
    this->clear();
    Context context(init);
    for (const WtoComponent<NodeId>& component : m_wto) {
      analyze_component(&context, component);
    }
  }

 private:
  void analyze_component(Context* context,
                         const WtoComponent<NodeId>& component) {
    if (component.is_vertex()) {
      this->analyze_vertex(context, component.head_node());
    } else {
      analyze_scc(context, component);
    }
  }

  void analyze_scc(Context* context, const WtoComponent<NodeId>& scc) {
    NodeId head = scc.head_node();
    bool iterate = true;
    for (context->reset_local_iteration_count_for(head); iterate;
         context->increase_iteration_count_for(head)) {
      this->analyze_vertex(context, head);
      for (const auto& component : scc) {
        analyze_component(context, component);
      }
      // The current state of the iteration is represented by a pointer to the
      // slot associated with the head node in the hash table of entry states.
      // The state is updated in place within the hash table via side effects,
      // which avoids costly copies and allocations.
      Domain* current_state = &this->m_entry_states[head];
      Domain new_state = Domain::bottom();
      this->compute_entry_state(context, head, &new_state);
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
        this->extrapolate(*context, head, current_state, new_state);
      }
    }
  }
  WeakTopologicalOrdering<NodeId, NodeHash> m_wto;
};

/*
 * Implementation of a deterministic concurrent fixpoint algorithm for weak
 * partial ordering (WPO) of a rooted directed graph, as described in the paper:
 *
 *   Sung Kook Kim, Arnaud J. Venet, and Aditya V. Thakur.
 *   Deterministic Parallel Fixpoint Computation.
 *
 *   https://dl.acm.org/ft_gateway.cfm?id=3371082
 *
 */
template <typename GraphInterface,
          typename Domain,
          typename NodeHash = std::hash<typename GraphInterface::NodeId>>
class ParallelMonotonicFixpointIterator
    : public fp_impl::
          MonotonicFixpointIteratorBase<GraphInterface, Domain, NodeHash> {
 public:
  using Graph = typename GraphInterface::Graph;
  using NodeId = typename GraphInterface::NodeId;
  using EdgeId = typename GraphInterface::EdgeId;
  using Context =
      fp_impl::MonotonicFixpointIteratorContext<NodeId, Domain, NodeHash>;
  using WPOWorkerState = WorkerState<uint32_t>;

  explicit ParallelMonotonicFixpointIterator(
      const Graph& graph, size_t num_thread = parallel::default_num_threads())
      : fp_impl::
            MonotonicFixpointIteratorBase<GraphInterface, Domain, NodeHash>(
                graph, /*cfg_size_hint*/ 4),
        m_wpo(
            GraphInterface::entry(graph),
            fp_impl::SuccessorNodeListBuilder<GraphInterface, NodeHash>(graph),
            false),
        m_num_thread(num_thread) {
    // Gathering all reachable nodes in graph.
    std::stack<NodeId> node_queue;
    node_queue.push(GraphInterface::entry(graph));
    m_all_nodes.emplace(GraphInterface::entry(graph));
    while (!node_queue.empty()) {
      auto node = node_queue.top();
      node_queue.pop();
      for (auto& edge : GraphInterface::successors(graph, node)) {
        auto target = GraphInterface::target(graph, edge);
        if (m_all_nodes.emplace(target).second) {
          node_queue.push(target);
        }
      }
    }
  }

  void set_all_to_bottom() {
    if (this->m_entry_states.size() < ChunkSize) {
      this->m_entry_states.reserve(m_all_nodes.size());
      this->m_exit_states.reserve(m_all_nodes.size());
      // Pre-populate entry and exit states for all nodes.
      for (auto& node : m_all_nodes) {
        this->m_entry_states[node] = Domain::bottom();
        this->m_exit_states[node] = Domain::bottom();
      }
      return;
    }

    assert(this->m_entry_states.size() == m_all_nodes.size());
    assert(this->m_exit_states.size() == m_all_nodes.size());
    // We are going to destroy a lot of domain values, which can be relatively
    // expensive. To speed this up, we are going to process chunks in
    // parallel.
    std::vector<Domain*> linear_map;
    linear_map.reserve(m_all_nodes.size() * 2);
    for (auto& [node, state] : this->m_entry_states) {
      linear_map.push_back(&state);
    }
    for (auto& [node, state] : this->m_exit_states) {
      linear_map.push_back(&state);
    }
    auto wq = sparta::work_queue<size_t>(
        [&linear_map](WorkerState<size_t>* worker_state, size_t start) {
          size_t end = std::min(linear_map.size(), start + ChunkSize);
          for (size_t i = start; i < end; i++) {
            *linear_map[i] = Domain::bottom();
          }
        });
    for (size_t i = 0; i < linear_map.size(); i += ChunkSize) {
      wq.add_item(i);
    }
    wq.run_all();
  }

  virtual ~ParallelMonotonicFixpointIterator() {
    if (this->m_entry_states.size() >= ChunkSize) {
      // We clear the memory explicitly so that we can do it faster in parallel.
      this->set_all_to_bottom();
    }
  }

  /*
   * Executes the fixpoint iterator given an abstract value describing the
   * initial program configuration. This method can be invoked multiple times
   * with different values in order to analyze the program under different
   * initial conditions.
   */
  void run(const Domain& init) {
    this->set_all_to_bottom();
    Context context(init, m_all_nodes);
    std::unique_ptr<std::atomic<uint32_t>[]> wpo_counter(
        new std::atomic<uint32_t>[m_wpo.size()]);
    std::fill_n(wpo_counter.get(), m_wpo.size(), 0);
    auto entry_idx = m_wpo.get_entry();
    assert(m_wpo.get_num_preds(entry_idx) == 0);
    // Prepare work queue.
    auto wq = sparta::work_queue<uint32_t>(
        [&context, &entry_idx, &wpo_counter, this](WPOWorkerState* worker_state,
                                                   uint32_t wpo_idx) {
          std::atomic<uint32_t>& current_counter = wpo_counter[wpo_idx];
          assert(current_counter == m_wpo.get_num_preds(wpo_idx));
          current_counter = 0;
          // NonExit node
          if (!m_wpo.is_exit(wpo_idx)) {
            this->analyze_vertex(&context, m_wpo.get_node(wpo_idx));
            for (auto succ_idx : m_wpo.get_successors(wpo_idx)) {
              std::atomic<uint32_t>& succ_counter = wpo_counter[succ_idx];
              // Increase succ node's counter, push succ nodes in work queue if
              // their counter number matches their NumSchedPreds.
              if (++succ_counter == m_wpo.get_num_preds(succ_idx)) {
                worker_state->push_task(succ_idx);
              }
            }
            return nullptr;
          }
          // Exit node
          // Check if component of the exit node has stabilized.
          auto head_idx = m_wpo.get_head_of_exit(wpo_idx);
          NodeId head = m_wpo.get_node(head_idx);
          Domain* current_state = &this->m_entry_states[head];
          Domain new_state = Domain::bottom();
          this->compute_entry_state(&context, head, &new_state);
          if (new_state.leq(*current_state)) {
            // Component stabilized.
            context.reset_local_iteration_count_for(head);
            *current_state = std::move(new_state);
            for (auto succ_idx : m_wpo.get_successors(wpo_idx)) {
              std::atomic<uint32_t>& succ_counter = wpo_counter[succ_idx];
              // Increase succ node's counter, push succ nodes in work queue if
              // their counter number matches their NumSchedPreds.
              if (++succ_counter == m_wpo.get_num_preds(succ_idx)) {
                worker_state->push_task(succ_idx);
              }
            }
          } else {
            // Component didn't stabilize.
            this->extrapolate(context, head, current_state, new_state);
            context.increase_iteration_count_for(head);
            // Set component nodes v's counter to their
            // NumOuterSchedPreds(v, wpo_idx)
            for (auto pred_pair : m_wpo.get_num_outer_preds(wpo_idx)) {
              auto component_idx = pred_pair.first;
              assert(component_idx != entry_idx);
              std::atomic<uint32_t>& component_counter =
                  wpo_counter[component_idx];
              // Push component nodes in work queue if their counter number
              // matches their NumSchedPreds.

              // Note: On page 10, https://dl.acm.org/ft_gateway.cfm?id=3371082
              // suggests to set the counter to be *equal* to the number of
              // predecessors not in our component. However, that is only
              // correct when all counter updates of a scheduling step are done
              // together as a single atomic update. Instead, we choose to
              // update point-wise, in which case we have to *add* the number of
              // predecessors, and update our own counter to 0 before updating
              // any other dependent counters.
              if ((component_counter += pred_pair.second) ==
                  m_wpo.get_num_preds(component_idx)) {
                worker_state->push_task(component_idx);
              }
            }
            if (head_idx == entry_idx) {
              // Handle special case when there is a loop on entry node.
              // Because entry node have num_preds = 0, and for
              // get_num_outer_preds the nodes with num_outer_preds are ignored.
              // So we need to manually add entry node back to work queue if
              // the component didn't stabilize.
              worker_state->push_task(head_idx);
            }
          }
          return nullptr;
        },
        m_num_thread,
        /*push_tasks_while_running=*/true);
    wq.add_item(m_wpo.get_entry());
    wq.run_all();
    for (uint32_t idx = 0; idx < m_wpo.size(); ++idx) {
      assert(wpo_counter[idx] == 0);
    }
  }

 private:
  WeakPartialOrdering<NodeId, NodeHash, /*Support_is_from_outside=*/false>
      m_wpo;
  size_t m_num_thread;
  std::unordered_set<NodeId> m_all_nodes;
  static constexpr size_t ChunkSize = 512;
};

/*
 * A sequential version of the fixpoint algorithm for Weak Partial Ordering.
 * Unlike the WTOMonotonicFixpointIterator, this does not rely on a recursive
 * algorithm to order its nodes, and so is not at risk of stack overflows.
 */
template <typename GraphInterface,
          typename Domain,
          typename NodeHash = std::hash<typename GraphInterface::NodeId>>
class MonotonicFixpointIterator
    : public fp_impl::
          MonotonicFixpointIteratorBase<GraphInterface, Domain, NodeHash> {
 public:
  using Graph = typename GraphInterface::Graph;
  using NodeId = typename GraphInterface::NodeId;
  using EdgeId = typename GraphInterface::EdgeId;
  using Context =
      fp_impl::MonotonicFixpointIteratorContext<NodeId, Domain, NodeHash>;

  explicit MonotonicFixpointIterator(const Graph& graph,
                                     size_t cfg_size_hint = 4)
      : fp_impl::MonotonicFixpointIteratorBase<GraphInterface,
                                               Domain,
                                               NodeHash>(graph, cfg_size_hint),
        m_wpo(
            GraphInterface::entry(graph),
            fp_impl::SuccessorNodeListBuilder<GraphInterface, NodeHash>(graph),
            false) {}

  /*
   * Executes the fixpoint iterator given an abstract value describing the
   * initial program configuration. This method can be invoked multiple times
   * with different values in order to analyze the program under different
   * initial conditions.
   */
  void run(const Domain& init) {
    this->clear();
    Context context(init);
    std::unique_ptr<std::atomic<uint32_t>[]> wpo_counter(
        new std::atomic<uint32_t>[m_wpo.size()]);
    std::fill_n(wpo_counter.get(), m_wpo.size(), 0);
    std::queue<uint32_t> work_queue;
    auto entry_idx = m_wpo.get_entry();
    assert(m_wpo.get_num_preds(entry_idx) == 0);
    // Prepare work queue.
    auto process_node = [&](uint32_t wpo_idx) {
      assert(wpo_counter[wpo_idx] == m_wpo.get_num_preds(wpo_idx));
      wpo_counter[wpo_idx] = 0;
      // NonExit node
      if (!m_wpo.is_exit(wpo_idx)) {
        this->analyze_vertex(&context, m_wpo.get_node(wpo_idx));
        for (auto succ_idx : m_wpo.get_successors(wpo_idx)) {
          // Increase succ node's counter, push succ nodes in work queue if
          // their counter number matches their NumSchedPreds.
          if (++wpo_counter[succ_idx] == m_wpo.get_num_preds(succ_idx)) {
            work_queue.emplace(succ_idx);
          }
        }
        return nullptr;
      }
      // Exit node
      // Check if component of the exit node has stabilized.
      uint32_t head_idx = m_wpo.get_head_of_exit(wpo_idx);
      NodeId head = m_wpo.get_node(head_idx);
      Domain* current_state = &this->m_entry_states[head];
      Domain new_state = Domain::bottom();
      this->compute_entry_state(&context, head, &new_state);
      if (new_state.leq(*current_state)) {
        // Component stabilized.
        context.reset_local_iteration_count_for(head);
        *current_state = std::move(new_state);
        for (auto succ_idx : m_wpo.get_successors(wpo_idx)) {
          // Increase succ node's counter, push succ nodes in work queue if
          // their counter number matches their NumSchedPreds.
          if (++wpo_counter[succ_idx] == m_wpo.get_num_preds(succ_idx)) {
            work_queue.emplace(succ_idx);
          }
        }
      } else {
        // Component didn't stabilize.
        this->extrapolate(context, head, current_state, new_state);
        context.increase_iteration_count_for(head);
        // Set component nodes v's counter to their
        // NumOuterSchedPreds(v, wpo_idx)
        for (auto pred_pair : m_wpo.get_num_outer_preds(wpo_idx)) {
          auto component_idx = pred_pair.first;
          assert(component_idx != entry_idx);
          // Push component nodes in work queue if their counter number
          // matches their NumSchedPreds.
          if ((wpo_counter[component_idx] += pred_pair.second) ==
              m_wpo.get_num_preds(component_idx)) {
            work_queue.emplace(component_idx);
          }
        }
        if (head_idx == entry_idx) {
          // Handle special case when there is a loop on entry node.
          // Because entry node have num_preds = 0, and for
          // get_num_outer_preds the nodes with num_outer_preds are ignored.
          // So we need to manually add entry node back to work queue if
          // the component didn't stabilize.
          work_queue.emplace(head_idx);
        }
      }
      return nullptr;
    };
    // Start from wpo entry node.
    work_queue.emplace(entry_idx);
    while (!work_queue.empty()) {
      auto item = work_queue.front();
      work_queue.pop();
      process_node(item);
    }
    for (uint32_t idx = 0; idx < m_wpo.size(); ++idx) {
      assert(wpo_counter[idx] == 0);
    }
  }

 private:
  WeakPartialOrdering<NodeId, NodeHash, /*Support_is_from_outside=*/false>
      m_wpo;
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
  static decltype(GraphInterface::successors(std::declval<const Graph&>(),
                                             std::declval<const NodeId&>()))
  predecessors(const Graph& graph, const NodeId& node) {
    return GraphInterface::successors(graph, node);
  }
  static decltype(GraphInterface::predecessors(std::declval<const Graph&>(),
                                               std::declval<const NodeId&>()))
  successors(const Graph& graph, const NodeId& node) {
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
