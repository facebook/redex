/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
#include "Show.h"
#include "Trace.h"
#include <algorithm>
#include <cmath>
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

LinearScanAllocator::LinearScanAllocator(DexMethod* method)
    : LinearScanAllocator(method->get_code(), is_static(method), [method]() {
        return show(method);
      }) {}

LinearScanAllocator::LinearScanAllocator(
    IRCode* code,
    bool is_static,
    const std::function<std::string()>& method_describer)
    : m_cfg(code), m_is_static(is_static) {
  TRACE(FREG,
        9,
        "Running FastRegAlloc for method {%s}",
        method_describer().c_str());
  if (code == nullptr) {
    return;
  }
  TRACE(FREG, 9, "[Original Code]\n%s", SHOW(code));

  live_range::renumber_registers(code, /* width_aware */ true);
  m_live_intervals = init_live_intervals(code, &m_live_interval_points);
  init_vreg_occurences();
  TRACE_live_intervals(m_live_intervals);
}

void LinearScanAllocator::allocate() {
  if (m_live_intervals.empty()) {
    return;
  }
  for (int idx = m_live_intervals.size() - 1; idx >= 0; --idx) {
    auto& live_interval = m_live_intervals[idx];
    expire_old_intervals(live_interval.end_point);
    // TODO: (in the future) add spill here given dex constraints
    vreg_t cur_vreg = live_interval.vreg;
    reg_t alloc_reg = allocate_register(cur_vreg, live_interval.end_point);
    live_interval.reg = alloc_reg;
    m_active_intervals.push(std::make_pair(idx, live_interval.start_point));
  }
  for (auto& interval : m_live_intervals) {
    auto& vreg_defs_uses = m_vreg_defs_uses[interval.vreg];
    for (auto def : vreg_defs_uses.first) {
      def->set_dest(interval.reg.value());
    }
    for (auto use : vreg_defs_uses.second) {
      use.insn->set_src(use.src_index, interval.reg.value());
    }
  }
  m_cfg->set_registers_size(m_reg_count);
  TRACE(FREG, 9, "FastRegAlloc pass complete!");
}

void LinearScanAllocator::init_vreg_occurences() {
  for (auto& mie : InstructionIterable(*m_cfg)) {
    auto insn = mie.insn;
    if (insn->has_dest()) {
      vreg_t dest_reg = insn->dest();
      if (insn->dest_is_wide()) {
        m_wide_vregs.insert(dest_reg);
      }
      m_vreg_defs_uses[dest_reg].first.push_back(insn);
      if (insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT && !m_is_static &&
          !m_this_vreg) {
        m_this_vreg = dest_reg;
      }
    }
  }
  for (auto& mie : InstructionIterable(*m_cfg)) {
    auto insn = mie.insn;
    for (src_index_t i = 0; i < insn->srcs_size(); ++i) {
      vreg_t src_reg = insn->src(i);
      always_assert(insn->src_is_wide(i) == m_wide_vregs.count(src_reg));
      m_vreg_defs_uses.at(src_reg).second.push_back(live_range::Use{insn, i});
    }
  }
}

reg_t LinearScanAllocator::allocate_register(reg_t for_vreg,
                                             uint32_t end_point) {
  bool wide = m_wide_vregs.count(for_vreg);
  if (!m_this_vreg || *m_this_vreg != for_vreg) {
    auto shape = IRInstructionShape::get(m_live_interval_points.at(end_point));
    auto& free_regs = m_free_regs[shape];
    for (auto it = free_regs.begin(); it != free_regs.end(); it++) {
      auto reg = *it;
      if (wide) {
        auto nit = std::next(it);
        if (nit == free_regs.end() || *nit != reg + 1) {
          continue;
        }
      }
      it = free_regs.erase(it);
      if (wide) {
        always_assert(*it == reg + 1);
        free_regs.erase(it);
      }
      return reg;
    }
  }

  auto alloc_reg = m_reg_count;
  m_reg_count += (wide ? 2 : 1);
  return alloc_reg;
}

void LinearScanAllocator::expire_old_intervals(uint32_t end_point) {
  while (!m_active_intervals.empty() &&
         m_active_intervals.top().second > end_point) {
    auto& interval_to_free = m_live_intervals[m_active_intervals.top().first];
    m_active_intervals.pop();
    if (!m_this_vreg || *m_this_vreg != interval_to_free.vreg) {
      reg_t freed_reg = interval_to_free.reg.value();
      auto shape = IRInstructionShape::get(
          m_live_interval_points.at(interval_to_free.end_point));
      auto& free_regs = m_free_regs[shape];
      bool success = free_regs.insert(freed_reg).second;
      always_assert(success);
      if (m_wide_vregs.count(interval_to_free.vreg)) {
        success = free_regs.insert(freed_reg + 1).second;
        always_assert(success);
      }
    }
  }
}

} // namespace fastregalloc
