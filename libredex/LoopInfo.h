/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <sparta/WeakTopologicalOrdering.h>

#include "ControlFlow.h"
#include "Debug.h"

#include <queue>

namespace loop_impl {

template <class NodeId>
using ComponentWrapper =
    std::reference_wrapper<const sparta::WtoComponent<NodeId>>;

/**
 * Visits a weak topological ordering component depth first search, applying
 * f to each basic block within the ordering
 */
template <class NodeId>
void visit_depth_first(const sparta::WtoComponent<NodeId>& comp,
                       std::function<void(NodeId)> f) {
  std::function<void(const sparta::WtoComponent<NodeId>&)> visit_component;
  visit_component = [&visit_component,
                     &f](const sparta::WtoComponent<NodeId>& v) {
    f(v.head_node());
    if (v.is_scc()) {
      for (const auto& inner : v) {
        visit_component(inner);
      }
    }
  };
  visit_component(comp);
}

/**
 * Constructs a level order traversal of the component heads of the provided
 * weak topological ordering and stores it in level_order.
 */
template <class NodeId>
void construct_level_order_traversal(
    std::vector<ComponentWrapper<NodeId>>& level_order,
    const sparta::WeakTopologicalOrdering<NodeId>& wto) {

  std::queue<ComponentWrapper<NodeId>> bfs_queue;

  // Get all of the outer SCCs by traversing the weak topological ordering.
  // For example, if the WTO looks like the following
  //
  //   1 2 (3 4 5 (6 7) 8) (9 10)
  //
  // bfs_queue will look like [3, 9].
  for (auto& vertex : wto) {
    if (vertex.is_scc()) {
      bfs_queue.push(vertex);
    }
  }

  // Run BFS on bfs_queue. We store the components in level order in the
  // level_order vector.
  while (!bfs_queue.empty()) {
    auto& vertex = bfs_queue.front().get();
    level_order.emplace_back(vertex);
    bfs_queue.pop();

    for (auto it = vertex.begin(); it != vertex.end(); ++it) {
      if (it->is_scc()) {
        bfs_queue.push(*it);
      }
    }
  }
}

/**
 * A cycle in the control flow graph: one strongly connected component of the
 * weak topological ordering, with nested components as subloops.
 *
 * Every cycle is a Loop, including one that can be entered at more than one
 * block -- a resume label inside a loop in a Kotlin suspend state machine is
 * the common source. Such a loop runs like any other, so a question about
 * whether a block repeats ("is it in a loop", "how deep") is answered by every
 * Loop. A question that needs a single entry -- a header that dominates the
 * body, a preheader that dominates the header -- holds only when
 * is_reducible() is true.
 */
class Loop {
 public:
  using iterator = std::vector<cfg::Block*>::iterator;
  using reverse_iterator = std::vector<cfg::Block*>::reverse_iterator;
  // `subloops` are the loops immediately nested in this one; the new loop
  // becomes their parent.
  Loop(const std::vector<cfg::Block*>& blocks,
       const UnorderedSet<Loop*>& subloops,
       cfg::Block* loop_preheader,
       bool is_reducible)
      : m_blocks(blocks),
        m_block_set(blocks.begin(), blocks.end()),
        m_loop_preheader(loop_preheader),
        m_parent_loop(nullptr),
        m_is_reducible(is_reducible) {
    // Immediate children only: each gets its parent once, in any order.
    for (auto* sub : UnorderedIterable(subloops)) {
      always_assert(sub->m_parent_loop == nullptr);
      sub->m_parent_loop = this;
    }
  }
  cfg::Block* get_header();
  cfg::Block* get_preheader();
  Loop* get_parent_loop();
  void set_preheader(cfg::Block* ph);
  bool contains(Loop* l) const;
  bool contains(cfg::Block* block) const;
  int get_loop_depth() const;
  bool is_reducible() const { return m_is_reducible; }
  UnorderedSet<cfg::Block*> get_exit_blocks() const;
  std::vector<cfg::Block*> get_blocks() const;

  iterator begin();
  iterator end();
  reverse_iterator rbegin();
  reverse_iterator rend();

 private:
  Loop(const Loop&) = delete;
  const Loop& operator=(const Loop&) = delete;
  std::vector<cfg::Block*> m_blocks;
  UnorderedSet<cfg::Block*> m_block_set;
  cfg::Block* m_loop_preheader;
  Loop* m_parent_loop;
  bool m_is_reducible;
};

class LoopInfo {
 public:
  using iterator = std::deque<Loop>::iterator;
  using reverse_iterator = std::deque<Loop>::reverse_iterator;
  explicit LoopInfo(const cfg::ControlFlowGraph& cfg);
  // Analyzes loops AND mutates `cfg`: inserts a dedicated preheader block
  // before the header of every reducible loop, redirecting external
  // predecessor edges through it. A loop with more than one entry gets none,
  // since no single block in front of it could dominate it. For analysis
  // only, use the const-ref constructor above.
  static LoopInfo make_with_preheaders(cfg::ControlFlowGraph& cfg);
  Loop* get_loop_for(cfg::Block* block);
  size_t num_loops();
  iterator begin();
  iterator end();
  reverse_iterator rbegin();
  reverse_iterator rend();

 private:
  // Tag for the private preheader-inserting constructor; use
  // make_with_preheaders().
  struct PreheaderTag {};
  LoopInfo(cfg::ControlFlowGraph& cfg, PreheaderTag);

  template <typename Cfg, typename Fn>
  void init(Cfg& cfg, Fn fn);

  std::deque<Loop> m_loops;
  UnorderedMap<cfg::Block*, int> m_loop_depth;
  UnorderedMap<cfg::Block*, Loop*> m_block_location;
};

} // namespace loop_impl
