/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>

#include <unordered_map>
#include <vector>

struct SimpleGraph {
  void add_edge(uint32_t pred, uint32_t succ) {
    succs[pred].push_back(succ);
    preds[succ].push_back(pred);
  }

  std::unordered_map<uint32_t, std::vector<uint32_t>> succs;
  std::unordered_map<uint32_t, std::vector<uint32_t>> preds;
};

class GraphInterface {
 public:
  using NodeId = uint32_t;
  using EdgeId = NodeId;
  using Graph = SimpleGraph;

  static NodeId entry(const Graph&) { return 0; }

  static std::vector<EdgeId> predecessors(const Graph& graph,
                                          const NodeId& node) {
    if (graph.preds.count(node)) {
      return graph.preds.at(node);
    }
    return {};
  }

  static std::vector<EdgeId> successors(const Graph& graph,
                                        const NodeId& node) {
    if (graph.succs.count(node)) {
      return graph.succs.at(node);
    }
    return {};
  }

  static NodeId source(const Graph&, const EdgeId& edge) { return edge; }

  static NodeId target(const Graph&, const EdgeId& edge) { return edge; }
};

class GraphInterfaceWithExit {
 public:
  using NodeId = uint32_t;
  using EdgeId = NodeId;
  using Graph = SimpleGraph;

  static NodeId entry(const Graph&) { return 0; }
  static NodeId exit(const Graph&) { return 100; }

  static std::vector<EdgeId> predecessors(const Graph& graph,
                                          const NodeId& node) {
    if (graph.preds.count(node)) {
      return graph.preds.at(node);
    }
    return {};
  }

  static std::vector<EdgeId> successors(const Graph& graph,
                                        const NodeId& node) {
    if (graph.succs.count(node)) {
      return graph.succs.at(node);
    }
    return {};
  }

  static NodeId source(const Graph&, const EdgeId& edge) { return edge; }

  static NodeId target(const Graph&, const EdgeId& edge) { return edge; }
};
