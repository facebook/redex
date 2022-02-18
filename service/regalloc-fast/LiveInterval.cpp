/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LiveInterval.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "DexUtil.h"
#include "LinearScan.h"
#include "ScopedCFG.h"

namespace fastregalloc {

LiveIntervals init_live_intervals(
    IRCode* code, std::vector<LiveIntervalPoint>* live_interval_points) {
  LiveIntervals live_intervals;
  VRegAliveInsns vreg_alive_insns;

  cfg::ScopedCFG cfg(code);
  cfg->simplify(); // in particular, remove empty blocks
  cfg->calculate_exit_block();
  LivenessFixpointIterator liveness_fixpoint_iter(*cfg);
  liveness_fixpoint_iter.run({});
  for (cfg::Block* block : cfg->blocks()) {
    auto vreg_block_range =
        get_live_range_in_block(liveness_fixpoint_iter, block);
    for (auto& pair : vreg_block_range) {
      vreg_t vreg = pair.first;
      RangeInBlock range = pair.second;
      vreg_alive_insns[vreg].push_back(range);
    }
  }

  // number the instructions to get sortable live intervals
  LiveIntervalPointIndices indices;
  auto ordered_blocks = get_ordered_blocks(*cfg, liveness_fixpoint_iter);
  for (auto block : ordered_blocks) {
    for (auto& mie : InstructionIterable(block)) {
      auto lip = LiveIntervalPoint::get(mie.insn);
      auto success = indices.emplace(lip, indices.size()).second;
      always_assert(success);
      live_interval_points->push_back(lip);
    }
    if (cfg->get_succ_edge_if(
            block, [](cfg::Edge* e) { return e->type() != cfg::EDGE_GHOST; })) {
      // Any block with continuing control-flow could have a live-out registers,
      // and thus we allocate a block-end point for it.
      auto lip = LiveIntervalPoint::get(block);
      auto success = indices.emplace(lip, indices.size()).second;
      always_assert(success);
      live_interval_points->push_back(lip);
    }
  }

  for (const auto& pair : vreg_alive_insns) {
    vreg_t vreg = pair.first;
    auto insn_ranges = pair.second;
    auto interval = calculate_live_interval(insn_ranges, indices);
    live_intervals.push_back(
        VRegLiveInterval{interval.first, interval.second, vreg, std::nullopt});
  }
  std::sort(live_intervals.begin(), live_intervals.end());

  return live_intervals;
}

IntervalEndPoints calculate_live_interval(
    std::vector<RangeInBlock>& ranges,
    const LiveIntervalPointIndices& indices) {
  VRegBlockRanges numbered_ranges;
  always_assert(!indices.empty());
  uint32_t max_index = indices.size() - 1;
  uint32_t interval_start = max_index;
  uint32_t interval_end = 0;
  for (auto& range : ranges) {
    always_assert(!range.first.is_missing());
    auto range_start = indices.at(range.first);
    interval_start = std::min(interval_start, range_start);
    // if there is deadcode (def no use), we assume the live interval lasts
    // until end of code
    if (range.second.is_missing()) {
      interval_end = max_index;
    } else {
      auto range_end = indices.at(range.second);
      interval_end = std::max(interval_end, range_end);
    }
  }
  redex_assert(interval_start <= interval_end);
  return std::make_pair(interval_start, interval_end);
}

VRegAliveRangeInBlock get_live_range_in_block(
    const LivenessFixpointIterator& fixpoint_iter, cfg::Block* block) {
  VRegAliveRangeInBlock vreg_block_range;
  LivenessDomain live_in = fixpoint_iter.get_live_in_vars_at(block);
  LivenessDomain live_out = fixpoint_iter.get_live_out_vars_at(block);

  auto first_insn_it = block->get_first_insn();
  LiveIntervalPoint first = first_insn_it == block->end()
                                ? LiveIntervalPoint::get(block)
                                : LiveIntervalPoint::get(first_insn_it->insn);
  for (auto vreg : live_in.elements()) {
    auto range = std::make_pair(first, LiveIntervalPoint::get());
    bool emplaced = vreg_block_range.emplace(vreg, range).second;
    always_assert(emplaced);
  }
  auto ii = InstructionIterable(block);
  for (auto it = ii.begin(); it != ii.end(); it++) {
    auto insn = it->insn;
    if (insn->has_dest()) {
      vreg_t vreg = insn->dest();
      auto next = std::next(it) == ii.end()
                      ? LiveIntervalPoint::get(block)
                      : LiveIntervalPoint::get(std::next(it)->insn);
      auto range = std::make_pair(next, LiveIntervalPoint::get());
      vreg_block_range.emplace(vreg, range);
      // emplace might silently fail if we already had an entry
    }
  }

  LiveIntervalPoint last = LiveIntervalPoint::get(block);
  for (auto vreg : live_out.elements()) {
    vreg_block_range.at(vreg).second = last;
  }
  for (auto it = block->rbegin(); it != block->rend(); ++it) {
    if (it->type != MFLOW_OPCODE) continue;
    auto insn = it->insn;
    for (vreg_t vreg : insn->srcs()) {
      auto it2 = vreg_block_range.find(vreg);
      if (it2 != vreg_block_range.end() && it2->second.second.is_missing()) {
        it2->second.second = LiveIntervalPoint::get(insn);
      }
    }
  }

  return vreg_block_range;
}

std::vector<cfg::Block*> get_ordered_blocks(
    cfg::ControlFlowGraph& cfg,
    const LivenessFixpointIterator& liveness_fixpoint_iter) {
  // For each block, compute distance (in number of blocks) from exit-block.
  std::unordered_map<cfg::Block*, size_t> block_depths;
  std::queue<std::pair<cfg::Block*, size_t>> work_queue;
  work_queue.emplace(cfg.exit_block(), 1);
  while (!work_queue.empty()) {
    auto [block, depth] = work_queue.front();
    work_queue.pop();
    if (!block_depths.emplace(block, depth).second) {
      continue;
    }
    for (auto e : block->preds()) {
      work_queue.emplace(e->src(), depth + 1);
    }
  }

  // Compute (maximum) depth (in number of blocks, from exit-block) of each
  // assigned register
  std::unordered_map<vreg_t, size_t> vreg_defs_depths;
  for (auto block : cfg.blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      if (mie.insn->has_dest()) {
        auto& depth = vreg_defs_depths[mie.insn->dest()];
        depth = std::max(depth, block_depths.at(block));
      }
    }
  }

  // For each block, compute the maximum distance (in number of blocks, from
  // exit-block) over all live-in registers
  std::unordered_map<cfg::Block*, size_t> live_in_def_depths;
  for (cfg::Block* block : cfg.blocks()) {
    auto live_in = liveness_fixpoint_iter.get_live_in_vars_at(block);
    size_t depth = 0;
    for (auto vreg : live_in.elements()) {
      auto vreg_defs_depth = vreg_defs_depths.at(vreg);
      depth = std::max(depth, vreg_defs_depth);
    }
    live_in_def_depths.emplace(block, depth);
  }

  // Collect blocks by doing a post-order traversal, processing predecessors in
  // their live-in-def-depths order, smallest depths goes last
  std::unordered_set<cfg::Block*> visited;
  std::vector<cfg::Block*> ordered_blocks;
  std::function<void(cfg::Block*)> visit;
  visit = [&](cfg::Block* block) {
    if (!visited.insert(block).second) {
      return;
    }
    std::vector<cfg::Block*> pred_blocks;
    for (auto e : block->preds()) {
      pred_blocks.push_back(e->src());
    }
    // We might have duplicates, but that's okay.
    std::sort(pred_blocks.begin(),
              pred_blocks.end(),
              [&live_in_def_depths](cfg::Block* a, cfg::Block* b) {
                auto a_depth = live_in_def_depths.at(a);
                auto b_depth = live_in_def_depths.at(b);
                if (a_depth != b_depth) {
                  return a_depth > b_depth;
                }
                return a->id() < b->id();
              });
    for (auto pred_block : pred_blocks) {
      visit(pred_block);
    }
    ordered_blocks.push_back(block);
  };
  visit(cfg.exit_block());
  always_assert(ordered_blocks.size() == cfg.blocks().size());
  return ordered_blocks;
}

} // namespace fastregalloc
