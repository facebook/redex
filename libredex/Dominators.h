/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <optional>

#include <sparta/MonotonicFixpointIterator.h>

#include "DeterministicContainers.h"
#include "GraphUtil.h"

namespace dominators {

template <class GraphInterface>
class SimpleFastDominators {
 public:
  using NodeId = typename GraphInterface::NodeId;

  /*
   * Find the immediate dominator for each node in the given graph. This
   * algorithm is described in the following paper:
   *
   *    K. D. Cooper et.al. A Simple, Fast Dominance Algorithm.
   */
  explicit SimpleFastDominators(const typename GraphInterface::Graph& graph) {
    // Sort nodes in postorder and create a map of each node to its postorder
    // number.
    m_postordering = graph::postorder_sort<GraphInterface>(graph);
    for (size_t i = 0; i < m_postordering.size(); ++i) {
      m_postorder_map[m_postordering[i]] = i;
    }
    // Entry node's immediate dominator is itself.
    const auto& entry = GraphInterface::entry(graph);
    m_idoms[entry] = entry;

    bool changed = true;
    while (changed) {
      changed = false;
      // Traverse nodes in reverse postorder.
      for (auto rit = m_postordering.rbegin(); rit != m_postordering.rend();
           ++rit) {
        NodeId node = *rit;
        if (node == entry) {
          continue;
        }
        const auto& preds = GraphInterface::predecessors(graph, node);
        std::optional<NodeId> new_idom;
        for (auto& pred : preds) {
          const auto& src = GraphInterface::source(graph, pred);
          if (!m_idoms.count(src)) {
            continue;
          }
          if (!new_idom) {
            new_idom = GraphInterface::source(graph, pred);
          } else {
            new_idom = intersect(*new_idom, src);
          }
        }
        always_assert(new_idom);
        if (!m_idoms.count(node) || m_idoms.at(node) != *new_idom) {
          m_idoms[node] = *new_idom;
          changed = true;
        }
      }
    }
  }

  NodeId get_idom(NodeId node) const { return m_idoms.at(node); }

  // Find the common dominator block that is closest to both blocks.
  NodeId intersect(NodeId finger1, NodeId finger2) {
    while (finger1 != finger2) {
      while (m_postorder_map.at(finger1) < m_postorder_map.at(finger2)) {
        finger1 = m_idoms.at(finger1);
      }
      while (m_postorder_map.at(finger2) < m_postorder_map.at(finger1)) {
        finger2 = m_idoms.at(finger2);
      }
    }
    return finger1;
  }

 private:
  UnorderedMap<NodeId, NodeId> m_idoms;
  std::vector<NodeId> m_postordering;
  UnorderedMap<NodeId, size_t> m_postorder_map;
};

// Post-dominators are just dominators over the reversed graph. sparta's
// BackwardsFixpointIterationAdaptor provides the reversal (swaps entry<->exit
// and pred<->succ), so this alias is the canonical, readable way to spell
// post-dominators instead of repeating the verbose adaptor at every call site
// (e.g. the std::conditional_t form in SourceBlockConsistencyCheck.h).
//
// PRECONDITION: the GraphInterface must expose an exit node. For
// `cfg::GraphInterface` that means the underlying `cfg::ControlFlowGraph` must
// have had `calculate_exit_block()` called. If it wasn't, the exit is nullptr
// and the constructor will dereference it (crash) or otherwise produce garbage
// — a recurring pitfall on freshly-built or freshly-mutated CFGs. Call
// `cfg.calculate_exit_block()` BEFORE constructing the post-dominators.
//
// Only nodes that can reach the exit are included; query get_idom() only
// for those (it throws std::out_of_range otherwise).
template <class GraphInterface>
using SimpleFastPostDominators = SimpleFastDominators<
    sparta::BackwardsFixpointIterationAdaptor<GraphInterface>>;

} // namespace dominators
