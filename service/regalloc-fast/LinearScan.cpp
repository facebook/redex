/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LinearScan.h"

#include "ControlFlow.h"
#include "CppUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "IRList.h"
#include "LiveInterval.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Trace.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fastregalloc {

static void TRACE_live_intervals(const LiveIntervals& live_intervals) {
  TRACE(FREG, 9, "[VReg Live Intervals]");
  if (traceEnabled(FREG, 9)) {
    for (auto interval_info : live_intervals) {
      TRACE(FREG, 9, "VReg name %d: ", interval_info.vreg);
      TRACE(FREG, 9, "Start point: %d", interval_info.start_point);
      TRACE(FREG, 9, "End point: %d", interval_info.end_point);
    }
  }
  TRACE(FREG, 9, "\n");
}

LinearScanAllocator::LinearScanAllocator(DexMethod* method) {
  TRACE(FREG, 9, "Running FastRegAlloc for method {%s}", SHOW(method));
  if (method->get_code() == nullptr) {
    return;
  }
  auto& code = *method->get_code();
  TRACE(FREG, 9, "[Original Code]\n%s", SHOW(&code));
  {
    // clear_cfg() called by ScopedCFG destructor will linearize the
    // instructions in the code
    cfg::ScopedCFG cfg_for_linearize = cfg::ScopedCFG(&code);
  }
  live_intervals = init_live_intervals(&code);
  init_vreg_occurences(&code);
  TRACE_live_intervals(live_intervals);
}

void LinearScanAllocator::allocate() {
  for (size_t idx = 0; idx < live_intervals.size(); ++idx) {
    expire_old_intervals(live_intervals[idx].start_point);
    reg_t alloc_reg;
    // TODO: (milestone 3) add spill here given dex constraints
    if (free_regs.empty()) {
      alloc_reg = reg_count;
      reg_count++;
    } else {
      alloc_reg = free_regs.front();
      free_regs.pop();
    }
    live_intervals[idx].reg = alloc_reg;
    active_intervals.push(std::make_pair(idx, live_intervals[idx].end_point));
    vreg_t cur_vreg = live_intervals[idx].vreg;
    for (auto def : vreg_defs_uses[cur_vreg].first) {
      def->set_dest(alloc_reg);
    }
    for (auto use : vreg_defs_uses[cur_vreg].second) {
      use.insn->set_src(use.src_index, alloc_reg);
    }
  }
  TRACE(FREG, 9, "FastRegAlloc pass complete!");
}

void LinearScanAllocator::init_vreg_occurences(IRCode* code) {
  code->build_cfg(/*editable*/ false);
  for (auto& mie : InstructionIterable(code)) {
    auto insn = mie.insn;
    if (insn->has_dest()) {
      vreg_t dest_reg = insn->dest();
      if (!vreg_defs_uses.count(dest_reg)) {
        vreg_defs_uses[dest_reg] =
            std::make_pair(std::vector<Def>({insn}), std::vector<Use>());
      } else {
        vreg_defs_uses[dest_reg].first.push_back(insn);
      }
    }
    auto srcs = insn->srcs_vec();
    for (src_index_t i = 0; i < srcs.size(); ++i) {
      vreg_t src_reg = srcs[i];
      if (!vreg_defs_uses.count(src_reg)) {
        vreg_defs_uses[src_reg] = std::make_pair(
            std::vector<Def>(), std::vector<Use>({live_range::Use{insn, i}}));
      } else {
        vreg_defs_uses[src_reg].second.push_back(live_range::Use{insn, i});
      }
    }
  }
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
