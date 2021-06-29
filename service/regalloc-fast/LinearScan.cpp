/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LinearScan.h"

#include "ControlFlow.h"
#include "IRInstruction.h"
#include "IRList.h"
#include "Show.h"
#include "Trace.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace fastregalloc {

static void TRACE_live_intervals(const LiveIntervals& live_intervals) {
  TRACE(FREG, 3, "[VReg Live Intervals]");
  if (traceEnabled(FREG, 3)) {
    for (auto interval_info : live_intervals) {
      TRACE(FREG, 3, "VReg name %d: ", interval_info.vreg);
      TRACE(FREG, 3, "Start point: %d", interval_info.first_def_idx);
      TRACE(FREG, 3, "End point: %d", interval_info.last_use_idx);
    }
  }
  TRACE(FREG, 3, "\n");
}

LinearScanAllocator::LinearScanAllocator(DexMethod* method) {
  auto code = method->get_code();
  TRACE(FREG, 2, "[Original Code]\n%s", SHOW(code));
  if (code) {
    init_vreg_info(code);
    TRACE_live_intervals(live_intervals);
  } else {
    TRACE(FREG, 3, "Empty code\n");
  }
}

void LinearScanAllocator::allocate() {
  for (size_t idx = 0; idx < live_intervals.size(); ++idx) {
    expire_old_intervals(live_intervals[idx].first_def_idx);
    reg_t alloc_reg;
    // TODO: (milestone 2) add spill here given total reg number
    if (free_regs.empty()) {
      alloc_reg = reg_count;
      reg_count++;
    } else {
      alloc_reg = free_regs.front();
      free_regs.pop();
    }
    live_intervals[idx].reg = alloc_reg;
    active_intervals.push(
        std::make_pair(idx, live_intervals[idx].last_use_idx));
    vreg_t cur_vreg = live_intervals[idx].vreg;
    for (auto def : vreg_defs_uses[cur_vreg].first) {
      def->set_dest(alloc_reg);
    }
    for (auto use : vreg_defs_uses[cur_vreg].second) {
      use.insn->set_src(use.src_index, alloc_reg);
    }
  }
}

void LinearScanAllocator::init_vreg_info(IRCode* code) {
  using FirstDefs = std::unordered_map<vreg_t, uint32_t>;

  uint32_t idx_count = 0;

  FirstDefs vreg_first_def;
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->has_dest()) {
      vreg_t dest_reg = insn->dest();
      if (!vreg_first_def.count(dest_reg)) {
        vreg_first_def[dest_reg] = idx_count;
        vreg_defs_uses[dest_reg] =
            std::make_pair(std::vector<Def>({insn}), std::vector<Use>());
      } else {
        vreg_defs_uses[dest_reg].first.push_back(insn);
      }
    }
    auto srcs = insn->srcs_vec();
    for (src_index_t i = 0; i < srcs.size(); ++i) {
      vreg_t src_reg = srcs[i];
      // first def always appear before uses
      assert(vreg_defs_uses.count(src_reg) == 1);
      vreg_defs_uses[src_reg].second.push_back(live_range::Use{insn, i});
    }
    idx_count++;
  }

  // reset idx_count to idx of the last insn
  idx_count--;

  for (auto it = code->rbegin(); it != code->rend(); ++it) {
    if (it->type != MFLOW_OPCODE) continue;
    auto insn = it->insn;
    auto srcs = insn->srcs_vec();
    for (src_index_t i = 0; i < srcs.size(); ++i) {
      vreg_t src_reg = srcs[i];
      if (vreg_first_def.count(src_reg)) {
        live_intervals.push_back(VRegLiveInterval{
            vreg_first_def[src_reg], idx_count, src_reg, std::nullopt});
        vreg_first_def.erase(src_reg); // avoid overwriting last use
      }
    }
    idx_count--;
  }

  // for no use vregs (dead code), we assume its live interval to be 1 insn
  if (!vreg_first_def.empty()) {
    for (auto remaining_def : vreg_first_def) {
      vreg_t vreg = remaining_def.first;
      uint32_t def_idx = remaining_def.second;
      live_intervals.push_back(VRegLiveInterval{def_idx, def_idx, vreg, 0});
    }
  }

  std::sort(live_intervals.begin(), live_intervals.end());
}

void LinearScanAllocator::expire_old_intervals(uint32_t cur_def_idx) {
  while (!active_intervals.empty() &&
         active_intervals.top().second < cur_def_idx) {
    auto interval_to_free = live_intervals[active_intervals.top().first];
    active_intervals.pop();
    try {
      reg_t freed_reg = interval_to_free.reg.value();
      free_regs.push(freed_reg);
    } catch (const std::bad_optional_access& e) {
      std::cerr << "Active interval ends with no register allocated: "
                << e.what() << std::endl;
    }
  }
}

} // namespace fastregalloc
