/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/optional/optional.hpp>
#include <unordered_map>

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
        boost::optional<NodeId> new_idom;
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
  std::unordered_map<NodeId, NodeId> m_idoms;
  std::vector<NodeId> m_postordering;
  std::unordered_map<NodeId, size_t> m_postorder_map;
};

} // namespace dominators
