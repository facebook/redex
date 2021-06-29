/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "DexClass.h"
#include "IRCode.h"
#include "LiveRange.h"
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>

namespace fastregalloc {

using vreg_t = uint16_t;

/*
 * Record the live interval (first def, last use) of a vreg. Also include the
 * vreg that owns each live interval, and the reg allocated to the vreg
 * (std::nullopt if not yet allocated).
 */
struct VRegLiveInterval {
  uint32_t first_def_idx;
  uint32_t last_use_idx;
  vreg_t vreg;
  std::optional<reg_t> reg;
  bool operator<(const VRegLiveInterval& vreg_live_interval) const {
    // at most one def per insn, first def idx all different
    return first_def_idx < vreg_live_interval.first_def_idx;
  }
};
/*
 * All virtual registers' live interval ordered by ascending first def insn idx
 * and ascending last use insn idx.
 */
using LiveIntervals = std::vector<VRegLiveInterval>;

/*
 * Group all defs and uses of a vreg.
 */
using Def = live_range::Def;
using Use = live_range::Use;
using VRegDefsUses =
    std::unordered_map<vreg_t, std::pair<std::vector<Def>, std::vector<Use>>>;

/*
 * Comparator for ActiveIntervals
 */
struct CmpActiveIntervalEndPoint {
  bool operator()(const std::pair<size_t, uint32_t>& lhs,
                  const std::pair<size_t, uint32_t>& rhs) {
    return lhs.second > rhs.second;
  }
};
/*
 * Order active intervals by their last use insn idx, desc.
 */
using ActiveIntervals =
    std::priority_queue<std::pair<size_t, uint32_t>,
                        std::vector<std::pair<size_t, uint32_t>>,
                        CmpActiveIntervalEndPoint>;

using FreeRegPool = std::queue<reg_t>;

/*
 * This implementation follows the pseudo algorithm proposed in paper Linear
 * Scan Register Allocation by M.Polleto and V. Sarkar
 * [https://dl.acm.org/doi/10.1145/330249.330250]
 */
class LinearScanAllocator final {
 public:
  explicit LinearScanAllocator(DexMethod* method);
  ~LinearScanAllocator() {}
  /*
   * For each live interval in ascending order of first def:
   * expire_old_intervals; check if anything in free queue, if so, allocate a
   * free reg to the vreg in current live interval; otherwise, allocate a new
   * reg, increase cur_max_reg.
   */
  void allocate();

 private:
  /*
   * interval -> vreg, reg
   */
  LiveIntervals live_intervals;

  /*
   * vreg -> all defs 7 uses
   * Group all defs and uses of the same vreg.
   */
  VRegDefsUses vreg_defs_uses;

  FreeRegPool free_regs;

  /*
   * { pair(interval_idx, last_use_idx) }
   * Current live intervals that has not reached their end point. i.e., live
   * intervals of active vregs.
   *
   * The reason we use "active intervals" instead of "active regs" is that
   * sorting intervals by last use idx can save work when checking for interval
   * expiration.
   */
  ActiveIntervals active_intervals;

  /*
   * Record the #reg allocated.
   */
  uint32_t reg_count = 0;

  /*
   * Set live_intervals and vreg_defs_uses recorded from ircode.
   * Forward traverse to get the first def of each vreg and all defs & uses,
   * then backward traverse to record the last use.
   */
  void init_vreg_info(IRCode* code);

  /*
   * Update free_regs and active_intervals: Check last use of each active
   * interval. If completed (last use < cur def), put the corresponding reg into
   * free queue; otherwise, do nothing.
   */
  void expire_old_intervals(uint32_t cur_def_idx);
};

} // namespace fastregalloc
