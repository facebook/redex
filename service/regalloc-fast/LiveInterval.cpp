/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LiveInterval.h"
#include "Show.h"
#include "Trace.h"
// #include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fastregalloc {

LiveIntervals init_live_intervals(IRCode* code) {
  LiveIntervals live_intervals;
  VRegAliveInsns vreg_alive_insns;

  code->build_cfg(/* editable */ true);
  auto& cfg = code->cfg();
  cfg.calculate_exit_block();
  LivenessFixpointIterator fixpoint_iter(cfg);
  fixpoint_iter.run(LivenessDomain());
  for (cfg::Block* block : cfg.blocks()) {
    auto vreg_block_range = get_live_range_in_block(fixpoint_iter, block);
    for (auto pair : vreg_block_range) {
      vreg_t vreg = pair.first;
      RangeInBlock range = pair.second;
      if (vreg_alive_insns.count(vreg)) {
        vreg_alive_insns[vreg].push_back(range);
      } else {
        vreg_alive_insns[vreg] = std::vector({range});
      }
    }
  }

  code->clear_cfg();

  // number the instructions to get sortable live intervals
  InsnIdx insn_idx;
  uint32_t idx_count = 0;
  code->build_cfg(/*editable*/ false);
  for (auto& mie : InstructionIterable(code)) {
    insn_idx[mie.insn] = idx_count;
    idx_count++;
  }

  for (const auto& pair : vreg_alive_insns) {
    vreg_t vreg = pair.first;
    auto insn_ranges = pair.second;
    auto interval =
        calculate_live_interval(insn_ranges, insn_idx, idx_count - 1);
    live_intervals.push_back(
        VRegLiveInterval{interval.first, interval.second, vreg, std::nullopt});
  }
  std::sort(live_intervals.begin(), live_intervals.end());

  return live_intervals;
}

IntervalEndPoints calculate_live_interval(
    std::vector<RangeInBlock>& insn_ranges,
    const InsnIdx& insn_idx,
    const uint32_t max_idx) {
  VRegBlockRanges numbered_ranges;
  uint32_t interval_start = max_idx;
  uint32_t interval_end = 0;
  for (auto range : insn_ranges) {
    if (insn_idx.at(range.first) < interval_start) {
      interval_start = insn_idx.at(range.first);
    }
    // if there is deadcode (def no use), we assume the live interval lasts
    // until end of code
    if (range.second == nullptr) {
      interval_end = max_idx;
    } else {
      if (insn_idx.at(range.second) > interval_end) {
        interval_end = insn_idx.at(range.second);
      }
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

  std::unordered_set<vreg_t> start_recorded;

  for (auto vreg : live_in.elements()) {
    vreg_block_range[vreg] =
        std::make_pair(block->get_first_insn()->insn, nullptr);
    start_recorded.insert(vreg);
  }
  for (auto& mie : InstructionIterable(block)) {
    auto insn = mie.insn;
    if (insn->has_dest()) {
      vreg_t vreg = insn->dest();
      if (!start_recorded.count(vreg)) {
        vreg_block_range[vreg].first = insn;
        start_recorded.insert(vreg);
      }
    }
  }

  for (auto vreg : live_out.elements()) {
    redex_assert(start_recorded.count(vreg) == 1);
    vreg_block_range[vreg].second = block->get_last_insn()->insn;
    start_recorded.erase(vreg);
  }
  for (auto it = block->rbegin(); it != block->rend(); ++it) {
    if (it->type != MFLOW_OPCODE) continue;
    auto insn = it->insn;
    for (vreg_t vreg : insn->srcs()) {
      if (start_recorded.count(vreg)) {
        vreg_block_range[vreg].second = insn;
        start_recorded.erase(vreg);
      }
    }
  }

  return vreg_block_range;
}

} // namespace fastregalloc
