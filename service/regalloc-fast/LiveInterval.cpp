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
    auto size = indices.size();
    for (auto& mie : InstructionIterable(block)) {
      auto lip = LiveIntervalPoint::get(mie.insn);
      auto success = indices.emplace(lip, indices.size()).second;
      always_assert(success);
      live_interval_points->push_back(lip);
    }
    if (size == indices.size() &&
        block->branchingness() != opcode::BRANCH_NONE) {
      // empty block that is not an unreachable ghost exit block
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

  LiveIntervalPoint first;
  LiveIntervalPoint last;
  auto first_insn_it = block->get_first_insn();
  if (first_insn_it == block->end()) {
    first = last = LiveIntervalPoint::get(block);
  } else {
    first = LiveIntervalPoint::get(first_insn_it->insn);
    auto last_insn_it = block->get_last_insn();
    always_assert(last_insn_it != block->end());
    last = LiveIntervalPoint::get(last_insn_it->insn);
  }
  always_assert(!first.is_missing());
  always_assert(!last.is_missing());
  for (auto vreg : live_in.elements()) {
    bool emplaced =
        vreg_block_range
            .emplace(vreg, std::make_pair(first, LiveIntervalPoint::get()))
            .second;
    always_assert(emplaced);
  }
  for (auto& mie : InstructionIterable(block)) {
    auto insn = mie.insn;
    if (insn->has_dest()) {
      vreg_t vreg = insn->dest();
      vreg_block_range.emplace(vreg,
                               std::make_pair(LiveIntervalPoint::get(insn),
                                              LiveIntervalPoint::get()));
      // emplace might silently fail if we already had an entry
    }
  }

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
