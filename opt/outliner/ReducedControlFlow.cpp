/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ReducedControlFlow.h"

#include <queue>

#include "CppUtil.h"
#include "WeakTopologicalOrdering.h"

namespace method_splitting_impl {

// Helper function that checks if a block is hit in any interaction.
bool is_hot(const cfg::Block* b) {
  const auto* sb = source_blocks::get_first_source_block(b);
  if (sb == nullptr) {
    return false;
  }

  bool is_hot = false;
  sb->foreach_val_early([&is_hot](const auto& val) {
    is_hot = (val && val->val > 0.0f);
    return is_hot;
  });

  return is_hot;
}

std::string_view describe(HotSplitKind kind) {
  switch (kind) {
  case HotSplitKind::Hot:
    return "hot";
  case HotSplitKind::HotCold:
    return "hot_cold";
  case HotSplitKind::Cold:
    return "cold";
  default:
    not_reached();
  }
}

std::vector<const cfg::Edge*> ReducedBlock::expand_preds(
    cfg::Block* src) const {
  std::vector<const cfg::Edge*> res;
  for (auto* reduced_edge : preds) {
    if (src == nullptr) {
      res.insert(res.end(), reduced_edge->edges.begin(),
                 reduced_edge->edges.end());
      continue;
    }
    for (auto* e : reduced_edge->edges) {
      if (e->src() == src) {
        res.push_back(e);
      }
    }
  }
  return res;
}

ReducedControlFlowGraph::ReducedControlFlowGraph(cfg::ControlFlowGraph& cfg)
    : m_cfg(cfg) {
  cfg.calculate_exit_block();
  sparta::WeakTopologicalOrdering<cfg::Block*> wto(
      cfg.entry_block(), [](cfg::Block* block) {
        std::vector<cfg::Block*> blocks;
        std::unordered_set<cfg::Block*> set;
        for (auto edge : block->succs()) {
          if (edge->target() != block && set.insert(edge->target()).second) {
            blocks.emplace_back(edge->target());
          }
        }
        return blocks;
      });
  for (auto& v : wto) {
    auto reduced_block = std::make_unique<ReducedBlock>();
    self_recursive_fn(
        [&blocks = reduced_block->blocks](const auto& self, const auto& w) {
          blocks.insert(w.head_node());
          if (!w.is_scc()) {
            return;
          }
          for (const auto& inner : w) {
            self(self, inner);
          }
        },
        v);
    reduced_block->id = m_reduced_blocks.size();
    for (auto* b : reduced_block->blocks) {
      auto [it, emplaced] = m_blocks.emplace(b, reduced_block.get());
      always_assert(emplaced);
    }
    m_reduced_blocks.push_back(std::move(reduced_block));
  }

  for (auto& reduced_block : m_reduced_blocks) {
    auto& blocks = reduced_block->blocks;
    for (auto* b : blocks) {
      for (auto* e : b->succs()) {
        if (e->target() && !blocks.count(e->target())) {
          always_assert(m_blocks.count(e->target()));
          auto reduced_edge =
              get_edge(reduced_block.get(), m_blocks.at(e->target()));
          reduced_block->succs.insert(reduced_edge);
          reduced_edge->edges.insert(e);
        }
      }
      for (auto* e : b->preds()) {
        if (e->src() && !blocks.count(e->src())) {
          always_assert(m_blocks.count(e->src()));
          auto reduced_edge =
              get_edge(m_blocks.at(e->src()), reduced_block.get());
          reduced_block->preds.insert(reduced_edge);
          reduced_edge->edges.insert(e);
        }
      }
      reduced_block->code_size += b->estimate_code_units();
      if (is_hot(b)) {
        reduced_block->is_hot = true;
      }
    }
  }
}

std::vector<const ReducedBlock*> ReducedControlFlowGraph::blocks() const {
  std::vector<const ReducedBlock*> res;
  res.reserve(m_reduced_blocks.size());
  for (auto& reduced_block : m_reduced_blocks) {
    res.push_back(reduced_block.get());
  }
  return res;
}

const ReducedBlock* ReducedControlFlowGraph::entry_block() const {
  always_assert(m_blocks.count(m_cfg.entry_block()));
  return m_blocks.at(m_cfg.entry_block());
}

std::unordered_set<const ReducedBlock*> ReducedControlFlowGraph::reachable(
    const ReducedBlock* head,
    const std::unordered_set<const ReducedEdge*>& except_edges) const {
  std::unordered_set<const ReducedBlock*> set;
  std::queue<const ReducedBlock*> work_queue;
  work_queue.push(head);
  while (!work_queue.empty()) {
    auto reduced_block = work_queue.front();
    work_queue.pop();
    if (set.insert(reduced_block).second) {
      for (auto* e : reduced_block->succs) {
        if (!except_edges.count(e)) {
          work_queue.push(e->target);
        }
      }
    }
  }
  return set;
}

ReducedBlock* ReducedControlFlowGraph::get_reduced_block(
    const cfg::Block* block) const {
  return m_blocks.at(block);
}

} // namespace method_splitting_impl
