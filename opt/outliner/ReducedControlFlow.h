/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ControlFlow.h"
#include "DexClass.h"
#include "SourceBlocks.h"

namespace method_splitting_impl {

// Helper function that checks if a block is hit in any interaction.
bool is_hot(const cfg::Block* b);

enum class HotSplitKind {
  Hot,
  HotCold,
  Cold,
};

std::string_view describe(HotSplitKind kind);

struct ReducedBlock;

struct ReducedEdge {
  const ReducedBlock* src{nullptr};
  const ReducedBlock* target{nullptr};
  std::unordered_set<const cfg::Edge*> edges{};
};

struct ReducedBlock {
  size_t id;
  std::unordered_set<const cfg::Block*> blocks;
  std::unordered_set<const ReducedEdge*> preds;
  std::unordered_set<const ReducedEdge*> succs;
  size_t code_size{0};
  bool is_hot{false};

  std::vector<const cfg::Edge*> expand_preds(cfg::Block* src = nullptr) const;
};

// A control-flow-graph where all strongly-connected components have been
// collapsed. Thus, this "graph" is really a DAG.
class ReducedControlFlowGraph {
 public:
  explicit ReducedControlFlowGraph(cfg::ControlFlowGraph& cfg);

  std::vector<const ReducedBlock*> blocks() const;

  const ReducedBlock* entry_block() const;

  std::unordered_set<const ReducedBlock*> reachable(
      const ReducedBlock* head,
      const std::unordered_set<const ReducedEdge*>& except_edges = {}) const;

  ReducedBlock* get_reduced_block(const cfg::Block* block) const;

 private:
  ReducedEdge* get_edge(ReducedBlock* src, ReducedBlock* target) {
    auto [it, _] =
        m_reduced_edges[src].emplace(target, ReducedEdge{src, target});
    return &it->second;
  }
  cfg::ControlFlowGraph& m_cfg;
  std::vector<std::unique_ptr<ReducedBlock>> m_reduced_blocks;
  std::unordered_map<ReducedBlock*,
                     std::unordered_map<ReducedBlock*, ReducedEdge>>
      m_reduced_edges;
  std::unordered_map<const cfg::Block*, ReducedBlock*> m_blocks;
};

template <class ReducedBlockCollection>
size_t code_size(const ReducedBlockCollection& blocks) {
  size_t res{0};
  for (const ReducedBlock* block : blocks) {
    res += block->code_size;
  }
  return res;
}

} // namespace method_splitting_impl
