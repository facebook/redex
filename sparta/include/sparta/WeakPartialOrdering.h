/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * Implementation of a weak partial ordering (WPO) of a rooted directed graph,
 * as described in the paper:
 *
 *   Sung Kook Kim, Arnaud J. Venet, and Aditya V. Thakur.
 *   Deterministic Parallel Fixpoint Computation.
 *
 *   https://dl.acm.org/ft_gateway.cfm?id=3371082
 *
 */

#pragma once

#include <boost/pending/disjoint_sets.hpp>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <set>
#include <stack>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sparta/Exceptions.h>

namespace sparta {

// Forward declarations
template <typename NodeId>
class WpoNode;

template <typename NodeId, typename NodeHash, bool Support_is_from_outside>
class WeakPartialOrdering;

namespace wpo_impl {

template <typename NodeId, typename NodeHash, bool Support_is_from_outside>
class WpoBuilder;

} // end namespace wpo_impl

/*
 * A node of a weak partial ordering.
 * It is either head, plain, or exit.
 */
template <typename NodeId>
class WpoNode final {
 public:
  enum class Type { Plain, Head, Exit };

 private:
  // Uses index of an array to id Wpo nodes.
  using WpoIdx = uint32_t;

 private:
  NodeId m_node;
  Type m_type;

  // Size of maximal SCC with this as its header.
  uint32_t m_size;
  // Successors of scheduling constraints.
  std::set<WpoIdx> m_successors;
  // Predecessors of scheduling constraints.
  std::set<WpoIdx> m_predecessors;
  // Number of outer predecessors w.r.t. the component (for exits only).
  std::unordered_map<WpoIdx, uint32_t> m_num_outer_preds;

 public:
  WpoNode(const NodeId& node, Type type, uint32_t size)
      : m_node(node), m_type(type), m_size(size) {}

 public:
  // Return the NodeId for this node
  const NodeId& get_node() const { return m_node; }

  // Check the type of this node
  bool is_plain() const { return m_type == Type::Plain; }
  bool is_head() const { return m_type == Type::Head; }
  bool is_exit() const { return m_type == Type::Exit; }

  // Get successors.
  const std::set<WpoIdx>& get_successors() const { return m_successors; }

  // Get predecessors.
  const std::set<WpoIdx>& get_predecessors() const { return m_predecessors; }

  // Get number of predecessors.
  uint32_t get_num_preds() const { return m_predecessors.size(); }

  // Get number of outer predecessors w.r.t. the component (for exits only).
  const std::unordered_map<WpoIdx, uint32_t>& get_num_outer_preds() const {
    SPARTA_RUNTIME_CHECK(m_type == Type::Exit, undefined_operation());
    return m_num_outer_preds;
  }

  // Get size of the SCC.
  uint32_t get_size() const { return m_size; }

 private:
  // Add successor.
  void add_successor(WpoIdx idx) { m_successors.insert(idx); }

  // Add predecessor.
  void add_predecessor(WpoIdx idx) { m_predecessors.insert(idx); }

  // Check if the given node is a successor.
  bool is_successor(WpoIdx idx) {
    return m_successors.find(idx) != m_successors.end();
  }

  // Increment the number of outer predecessors.
  void inc_num_outer_preds(WpoIdx idx) {
    SPARTA_RUNTIME_CHECK(m_type == Type::Exit, internal_error());
    m_num_outer_preds[idx]++;
  }

 public:
  WpoNode(WpoNode&&) = default;
  WpoNode(const WpoNode&) = delete;
  WpoNode& operator=(const WpoNode&) = delete;
  WpoNode& operator=(WpoNode&&) = delete;

  template <typename T1, typename T2, bool B>
  friend class wpo_impl::WpoBuilder;
}; // end class WpoNode

/*
 * Implementation of the decomposition of a rooted directed graph into a weak
 * partial ordering (WPO). A technical paper to appear in POPL 2020:
 *
 *   Sung Kook Kim, Arnaud J. Venet, and Aditya V. Thakur.
 *   Deterministic Parallel Fixpoint Computation.
 *
 *   https://dl.acm.org/ft_gateway.cfm?id=3371082
 *
 */
template <typename NodeId,
          typename NodeHash = std::hash<NodeId>,
          bool Support_is_from_outside = true>
class WeakPartialOrdering final {
 private:
  using WpoNodeT = WpoNode<NodeId>;
  using Type = typename WpoNodeT::Type;
  using WpoIdx = uint32_t;

 private:
  // WPO nodes.
  std::vector<WpoNodeT> m_nodes;
  // Top level nodes. Nodes that are outside of any component.
  // This is NOT used in the concurrent fixpoint iteration.
  std::vector<WpoIdx> m_toplevel;
  // post DFN of the nodes.
  std::unordered_map<NodeId, uint32_t, NodeHash> m_post_dfn;
  // This is used when generating a WTO from a WPO.
  // See algorithm ConstructWTO^{BU} in Section 7 of the POPL 2020 paper.
  bool m_lifted;

  // Get the post DFN of a node.
  uint32_t get_post_dfn(NodeId n) const {
    auto it = m_post_dfn.find(n);
    if (it != m_post_dfn.end()) {
      return it->second;
    }
    return 0;
  }

 public:
  // Construct a WPO for the given CFG.
  WeakPartialOrdering(
      const NodeId& root,
      std::function<std::vector<NodeId>(const NodeId&)> successors,
      bool lift)
      : m_lifted(lift) {
    if (successors(root).empty()) {
      m_nodes.emplace_back(root, Type::Plain, /*size=*/1);
      m_toplevel.push_back(0);
      m_post_dfn[root] = 1;
      return;
    }
    wpo_impl::WpoBuilder<NodeId, NodeHash, Support_is_from_outside> builder(
        successors, m_nodes, m_toplevel, m_post_dfn, lift);
    builder.build(root);
  }

  // Total number of nodes in this wpo.
  uint32_t size() const { return m_nodes.size(); }

  // Entry node of this wpo.
  WpoIdx get_entry() { return m_nodes.size() - 1; }

  // Successors of the node.
  const std::set<WpoIdx>& get_successors(WpoIdx idx) const {
    return m_nodes[idx].get_successors();
  }

  // Predecessors of the node.
  const std::set<WpoIdx>& get_predecessors(WpoIdx idx) const {
    return m_nodes[idx].get_predecessors();
  }

  // Number of predecessors of the node.
  uint32_t get_num_preds(WpoIdx idx) const {
    return m_nodes[idx].get_num_preds();
  }

  // Get number of outer preds for the exit's component.
  const std::unordered_map<WpoIdx, uint32_t>& get_num_outer_preds(
      WpoIdx exit) const {
    return m_nodes[exit].get_num_outer_preds();
  }

  // Head of the exit node.
  WpoIdx get_head_of_exit(WpoIdx exit) const { return exit + 1; }

  // Exit of the head node.
  WpoIdx get_exit_of_head(WpoIdx head) const { return head - 1; }

  // NodeId for the node.
  const NodeId& get_node(WpoIdx idx) const { return m_nodes[idx].get_node(); }

  // Type queries for node.
  bool is_plain(WpoIdx idx) const { return m_nodes[idx].is_plain(); }
  bool is_head(WpoIdx idx) const { return m_nodes[idx].is_head(); }
  bool is_exit(WpoIdx idx) const { return m_nodes[idx].is_exit(); }

  // Check whether a predecessor is outside of the component of the head.
  // This can be used to detect whether an edge is a back edge:
  //   is_backedge(head, pred)
  //     := !is_from_outside(head, pred) /\ is_predecessor(head, pred)
  // This is used in interleaved widening and narrowing.
  bool is_from_outside(NodeId head, NodeId pred) {
    SPARTA_RUNTIME_CHECK(Support_is_from_outside, undefined_operation());
    return get_post_dfn(head) < get_post_dfn(pred);
  }

  WeakPartialOrdering(const WeakPartialOrdering& other) = delete;
  WeakPartialOrdering(WeakPartialOrdering&& other) = delete;
  WeakPartialOrdering& operator=(const WeakPartialOrdering& other) = delete;
  WeakPartialOrdering& operator=(WeakPartialOrdering&& other) = delete;
}; // end class WeakPartialOrdering

namespace wpo_impl {

template <class T>
struct VectorMap {
 private:
  std::vector<T> m_vec;

  void recalibrate(size_t i) {
    if (i >= m_vec.size()) {
      m_vec.resize(i * 2 + 1);
    }
  }

 public:
  T& operator[](size_t i) {
    recalibrate(i);
    return m_vec[i];
  }

  void set(size_t i, T value) {
    recalibrate(i);
    m_vec[i] = std::move(value);
  }

  T get(size_t i) const { return i >= m_vec.size() ? T() : m_vec[i]; }

  const T* get_opt(size_t i) const {
    return i >= m_vec.size() ? nullptr : &m_vec[i];
  }
};

template <typename NodeId, typename NodeHash, bool Support_is_from_outside>
class WpoBuilder final {
 private:
  using WpoNodeT = WpoNode<NodeId>;
  using WpoT = WeakPartialOrdering<NodeId, NodeHash, Support_is_from_outside>;
  using Type = typename WpoNodeT::Type;
  using WpoIdx = uint32_t;

 public:
  WpoBuilder(std::function<std::vector<NodeId>(const NodeId&)> successors,
             std::vector<WpoNodeT>& wpo_space,
             std::vector<WpoIdx>& toplevel,
             std::unordered_map<NodeId, uint32_t, NodeHash>& post_dfn,
             bool lift)
      : m_successors(successors),
        m_wpo_space(wpo_space),
        m_toplevel(toplevel),
        m_post_dfn(post_dfn),
        m_next_dfn(1),
        m_next_post_dfn(1),
        m_next_idx(0),
        m_lift(lift) {}

  void build(const NodeId& root) {
    construct_auxilary(root);
    construct_wpo();
    // Compute num_outer_preds.
    for (auto& p : m_for_outer_preds) {
      auto& v = p.first;
      auto& x_max = p.second;
      auto h = m_wpo_space[v].is_head() ? v : m_parent[v];
      // index of exit == index of head - 1.
      auto x = h - 1;
      while (x != x_max) {
        m_wpo_space[x].inc_num_outer_preds(v);
        h = m_parent[h];
        x = h - 1;
      }
      m_wpo_space[x].inc_num_outer_preds(v);
    }
  }

 private:
  // Construct auxilary data-structures.
  // Performs DFS iteratively to classify the edges and find lowest common
  // ancestors of cross/forward edges.
  // Nodes are identified by their DFNs in the builder.
  void construct_auxilary(const NodeId& root) {
    // It uses disjoint-sets data structure.
    typedef std::unordered_map<uint32_t, std::size_t> rank_t;
    typedef std::unordered_map<uint32_t, uint32_t> parent_t;
    rank_t rank_map;
    parent_t parent_map;
    typedef boost::associative_property_map<rank_t> r_pmap_t;
    r_pmap_t r_pmap(rank_map);
    typedef boost::associative_property_map<parent_t> p_pmap_t;
    p_pmap_t p_pmap(parent_map);
    boost::disjoint_sets<r_pmap_t, p_pmap_t> dsets(r_pmap, p_pmap);

    VectorMap<uint32_t> ancestor;
    struct StackEntry {
      NodeId vertex_ref;
      uint32_t pred;
      uint32_t finished_vertex;
    };
    std::stack<StackEntry> stack;
    VectorMap<bool> black;

    stack.push(StackEntry{root, 0, 0});
    while (!stack.empty()) {
      // Iterative DFS.
      auto [vertex_ref, pred, finished_vertex] = stack.top();
      stack.pop();

      if (finished_vertex != 0) {
        if (Support_is_from_outside) {
          // DFS is done with this vertex.
          // Set the post DFN.
          m_post_dfn[vertex_ref] = m_next_post_dfn++;
        }

        // Mark visited.
        black.set(finished_vertex, true);

        dsets.union_set(finished_vertex, pred);
        ancestor[dsets.find_set(pred)] = pred;
      } else {
        auto& vertex = m_dfn[vertex_ref];
        if (vertex != 0 /* means that the vertex is already discovered. */) {
          // A forward edge.
          // Forward edges can be ignored, as they are redundant.
          continue;
        }
        // New vertex is discovered.
        vertex = m_next_dfn++;
        push_ref(vertex_ref);
        dsets.make_set(vertex);

        // This will be popped after all its successors are finished.
        stack.push((StackEntry){vertex_ref, pred, vertex});

        auto successors = m_successors(vertex_ref);
        // Successors vector is reversed to match the order with WTO.
        for (auto rit = successors.rbegin(); rit != successors.rend(); ++rit) {
          auto succ = get_dfn(*rit);
          if (succ == 0 /* 0 means that vertex is undiscovered. */) {
            // Newly discovered vertex. Search continues.
            stack.push((StackEntry){*rit, vertex, 0});
          } else if (black.get(succ)) {
            // A cross edge.
            auto lca = ancestor[dsets.find_set(succ)];
            m_cross_fwds[lca].emplace_back(vertex, succ);
          } else {
            // A back edge.
            m_back_preds[succ].push_back(vertex);
          }
        }
        if (pred != 0) {
          // A tree edge.
          m_non_back_preds[vertex].push_back(pred);
        }
      }
    }
  }

  void construct_wpo() {
    std::vector<uint32_t> rank(get_next_dfn());
    std::vector<uint32_t> parent(get_next_dfn());
    // A partition of vertices. Each subset is known to be strongly connected.
    boost::disjoint_sets<uint32_t*, uint32_t*> dsets(&rank[0], &parent[0]);
    // Maps representative of a set to the vertex with minimum DFN.
    std::vector<uint32_t> rep(get_next_dfn());
    // Maps a head to its exit.
    std::vector<uint32_t> exit(get_next_dfn());
    // Maps a vertex to original non-back edges that now target the vertex.
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> origin(
        get_next_dfn());
    // Maps a head to its size of components.
    std::vector<uint32_t> size(get_next_dfn());
    // Index of WpoNode in wpo space.
    m_d2i.resize(2 * get_next_dfn());
    // DFN that will be assigned to the next exit.
    uint32_t dfn = get_next_dfn();

    // Initialization.
    for (uint32_t v = 1; v < get_next_dfn(); v++) {
      dsets.make_set(v);
      rep[v] = exit[v] = v;
      const auto& non_back_preds_v = m_non_back_preds[v];
      auto& origin_v = origin[v];
      origin_v.reserve(origin_v.size() + non_back_preds_v.size());
      for (auto u : non_back_preds_v) {
        origin_v.emplace_back(u, v);
      }
    }
    // In reverse DFS order, build WPOs for SCCs bottom-up.
    for (uint32_t h = get_next_dfn() - 1; h > 0; h--) {
      // Restore cross/fwd edges which has h as the LCA.
      auto opt = m_cross_fwds.get_opt(h);
      if (opt != nullptr) {
        for (auto& edge : *opt) {
          // edge: u -> v
          auto& u = edge.first;
          auto& v = edge.second;
          auto rep_v = rep[dsets.find_set(v)];
          m_non_back_preds[rep_v].push_back(u);
          origin[rep_v].emplace_back(u, v);
        }
      }

      // Find nested SCCs.
      bool is_SCC = false;
      std::unordered_set<uint32_t> backpreds_h;
      for (auto v : m_back_preds[h]) {
        if (v != h) {
          backpreds_h.insert(rep[dsets.find_set(v)]);
        } else {
          // Self-loop.
          is_SCC = true;
        }
      }
      if (!backpreds_h.empty()) {
        is_SCC = true;
      }
      // Invariant: h \notin backpreds_h.
      std::unordered_set<uint32_t> nested_SCCs_h(backpreds_h);
      std::vector<uint32_t> worklist_h(backpreds_h.begin(), backpreds_h.end());
      while (!worklist_h.empty()) {
        auto v = worklist_h.back();
        worklist_h.pop_back();
        for (auto p : m_non_back_preds[v]) {
          auto rep_p = rep[dsets.find_set(p)];
          auto p_it = nested_SCCs_h.find(rep_p);
          if (p_it == nested_SCCs_h.end() && rep_p != h) {
            nested_SCCs_h.insert(rep_p);
            worklist_h.push_back(rep_p);
          }
        }
      }
      // Invariant: h \notin nested_SCCs_h.

      // h is a Trivial SCC.
      if (!is_SCC) {
        size[h] = 1;
        add_node(h, get_ref(h), Type::Plain, /*size=*/1);
        // Invariant: wpo_space = ...::h
        continue;
      }

      // Compute the size of the component C_h.
      // Size of this component is initialized to 2: the head and the exit.
      uint32_t size_h = 2;
      for (auto v : nested_SCCs_h) {
        size_h += size[v];
      }
      size[h] = size_h;
      // Invariant: size_h = size[h] = number of nodes in the component C_h.

      // Add new exit x_h.
      auto x_h = dfn++;
      add_node(x_h, get_ref(h), Type::Exit, size_h);
      add_node(h, get_ref(h), Type::Head, size_h);
      // Invariant: wpo_space = ...::x_h::h
      if (backpreds_h.empty()) {
        // Add scheduling constraints from h to x_h
        add_successor(/*from=*/h,
                      /*to=*/x_h,
                      /*exit=*/x_h,
                      /*outer_pred?=*/false);
      } else {
        for (auto p : backpreds_h) {
          add_successor(/*from=*/exit[p],
                        /*to=*/x_h,
                        /*exit=*/x_h,
                        /*outer_pred?=*/false);
        }
      }
      // Invariant: Scheduling contraints to x_h are all constructed.

      // Add scheduling constraints between the WPOs for nested SCCs.
      for (auto v : nested_SCCs_h) {
        for (auto& edge : origin[v]) {
          auto& u = edge.first;
          auto& vv = edge.second;
          auto& x_u = exit[rep[dsets.find_set(u)]];
          auto& x_v = exit[v];
          // Invariant: u -> vv, u \notin C_v, vv \in C_v, u \in C_h, v \in C_h.
          if (m_lift) {
            add_successor(/*from=*/x_u,
                          /*to=*/v,
                          /*exit=*/x_v,
                          /*outer_pred?=*/x_v != v);
            // Invariant: x_u \in get_predecessors(v).
          } else {
            add_successor(/*from=*/x_u,
                          /*to=*/vv,
                          /*exit=*/x_v,
                          /*outer_pred?=*/x_v != v);
            // Invariant: x_u \in get_predecessors(vv).
          }
        }
      }
      // Invariant: WPO for SCC with h as its header is constructed.

      // Update the partition by merging.
      for (auto v : nested_SCCs_h) {
        dsets.union_set(v, h);
        rep[dsets.find_set(v)] = h;
        m_parent[index_of(v)] = index_of(h);
      }

      // Set exit of h to x_h.
      exit[h] = x_h;
      // Invariant: exit[h] = h if C_h is trivial SCC, x_h otherwise.
    }

    // Add scheduling constraints between the WPOs for maximal SCCs.
    m_toplevel.reserve(get_next_dfn());
    for (uint32_t v = 1; v < get_next_dfn(); v++) {
      if (rep[dsets.find_set(v)] == v) {
        add_toplevel(v);
        m_parent[index_of(v)] = index_of(v);

        for (auto& edge : origin[v]) {
          auto& u = edge.first;
          auto& vv = edge.second;
          auto& x_u = exit[rep[dsets.find_set(u)]];
          auto& x_v = exit[v];
          // Invariant: u -> vv, u \notin C_v, vv \in C_v, u \in C_h, v \in C_h.
          if (m_lift) {
            add_successor(/*from=*/x_u,
                          /*to=*/v,
                          /*exit=*/x_v,
                          /*outer_pred?=*/x_v != v);
            // Invariant: x_u \in get_predecessors(v).
          } else {
            add_successor(/*from=*/x_u,
                          /*to=*/vv,
                          /*exit=*/x_v,
                          /*outer_pred?=*/x_v != v);
            // Invariant: x_u \in get_predecessors(vv).
          }
        }
      }
    }
    // Invariant: WPO for the CFG is constructed.
  }

  uint32_t get_dfn(NodeId n) {
    auto it = m_dfn.find(n);
    if (it != m_dfn.end()) {
      return it->second;
    }
    return 0;
  }

  const NodeId& get_ref(uint32_t num) const { return m_ref.at(num - 1); }

  void push_ref(NodeId n) { m_ref.push_back(n); }

  uint32_t get_next_dfn() const {
    // Includes exits.
    // 0 represents invalid node.
    return m_next_dfn;
  }

  void add_node(uint32_t dfn, NodeId ref, Type type, uint32_t size) {
    m_d2i[dfn] = m_next_idx++;
    m_wpo_space.emplace_back(ref, type, size);
  }

  WpoNodeT& node_of(uint32_t dfn) { return m_wpo_space[index_of(dfn)]; }

  WpoIdx index_of(uint32_t dfn) { return m_d2i[dfn]; }

  void add_successor(uint32_t from,
                     uint32_t to,
                     uint32_t exit,
                     bool outer_pred) {
    auto fromIdx = index_of(from);
    auto toIdx = index_of(to);
    auto& fromNode = node_of(from);
    auto& toNode = node_of(to);
    if (!fromNode.is_successor(toIdx)) {
      if (outer_pred) {
        m_for_outer_preds.push_back(std::make_pair(toIdx, index_of(exit)));
      }
      fromNode.add_successor(toIdx);
      toNode.add_predecessor(fromIdx);
    }
  }

  void add_toplevel(uint32_t what) { m_toplevel.push_back(index_of(what)); }

  std::function<std::vector<NodeId>(const NodeId&)> m_successors;
  // A reference to Wpo space (array of Wpo nodes).
  std::vector<WpoNodeT>& m_wpo_space;
  // A reference to Wpo space that contains only the top level nodes.
  std::vector<WpoIdx>& m_toplevel;
  // A map from NodeId to DFN.
  std::unordered_map<NodeId, uint32_t, NodeHash> m_dfn;
  // A map from NodeId to post DFN.
  std::unordered_map<NodeId, uint32_t, NodeHash>& m_post_dfn;
  // A map from DFN to NodeId.
  std::vector<NodeId> m_ref;
  // A map from DFN to DFNs of its backedge predecessors.
  VectorMap<std::vector<uint32_t>> m_back_preds;
  // A map from DFN to DFNs of its non-backedge predecessors.
  VectorMap<std::vector<uint32_t>> m_non_back_preds;
  // A map from DFN to cross/forward edges (DFN is the lowest common ancestor).
  VectorMap<std::vector<std::pair<uint32_t, uint32_t>>> m_cross_fwds;
  // Increase m_num_outer_preds[x][pair.first] for component C_x that satisfies
  // pair.first \in C_x \subseteq C_{pair.second}.
  std::vector<std::pair<WpoIdx, WpoIdx>> m_for_outer_preds;
  // A map from node to the head of minimal component that contains it as
  // non-header.
  std::unordered_map<WpoIdx, WpoIdx> m_parent;
  // Maps DFN to WpoIdx.
  std::vector<uint32_t> m_d2i;
  // Next DFN to assign.
  uint32_t m_next_dfn;
  // Next post DFN to assign.
  uint32_t m_next_post_dfn;
  // Next WpoIdx to assign.
  uint32_t m_next_idx;
  // 'Lift' the scheduling constraints when adding them.
  bool m_lift;
}; // end class wpo_builder

} // end namespace wpo_impl

} // end namespace sparta
