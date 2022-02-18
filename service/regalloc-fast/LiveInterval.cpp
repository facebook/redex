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
#include "MonotonicFixpointIterator.h"
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
  for (auto block : cfg->blocks()) {
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

} // namespace fastregalloc
