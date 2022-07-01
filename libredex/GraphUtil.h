/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <vector>

#include "Debug.h"

namespace graph {

/*
 * Iterative implementation of a postorder sort.
 */
template <class GraphInterface>
std::vector<typename GraphInterface::NodeId> postorder_sort(
    const typename GraphInterface::Graph& graph) {
  using NodeId = typename GraphInterface::NodeId;
  enum State { UNVISITED, VISITING, VISITED };
  NodeId entry = GraphInterface::entry(graph);
  std::vector<NodeId> stack{entry};
  std::unordered_map<NodeId, State> states{{entry, UNVISITED}};
  std::vector<NodeId> postorder;

  while (!stack.empty()) {
    const auto& curr = stack.back();
    auto state_it = states.find(curr);
    always_assert(state_it != states.end());
    switch (state_it->second) {
    case UNVISITED: {
      state_it->second = VISITING;
      for (auto const& s : GraphInterface::successors(graph, curr)) {
        const auto& target = GraphInterface::target(graph, s);
        if (!states.count(target)) {
          states[target] = UNVISITED;
          stack.push_back(target);
        }
      }
      break;
    }
    case VISITING: {
      state_it->second = VISITED;
      postorder.push_back(curr);
      stack.pop_back();
      break;
    }
    case VISITED: {
      not_reached();
    }
    }
  }

  return postorder;
}

} // namespace graph
