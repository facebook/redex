/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <queue>

#include "DeterministicContainers.h"
#include "DexClass.h"
#include "IRCode.h"
#include "LiveRange.h"
#include "ScopedCFG.h"

namespace fastregalloc {

using vreg_t = uint16_t;

/*
 * Record the live interval (first def/use, last def/use) of a vreg. Also
 * include the vreg that owns each live interval, and the reg allocated to the
 * vreg (std::nullopt if not yet allocated).
 */
struct VRegLiveInterval {
  uint32_t start_point;
  uint32_t end_point;
  vreg_t vreg;
  std::optional<reg_t> reg{std::nullopt};
  bool operator<(const VRegLiveInterval& vreg_live_interval) const {
    if (end_point != vreg_live_interval.end_point) {
      return end_point < vreg_live_interval.end_point;
    }
    if (start_point != vreg_live_interval.start_point) {
      return start_point < vreg_live_interval.start_point;
    }
    // We might have live-interval with the same start- and end-points, due to
    // auxiliary live-intervals created to represent the check-cast quirk. We
    // disambiguate those last.
    return vreg < vreg_live_interval.vreg;
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
    UnorderedMap<vreg_t, std::pair<std::vector<Def>, std::vector<Use>>>;

using WideVReg = UnorderedSet<vreg_t>;

/*
 * ActiveInterval, ordered first by start_point, and then by live_interval_idx.
 */
struct ActiveInterval {
  int32_t live_interval_idx;
  uint32_t start_point;
  bool operator<(const ActiveInterval& other) const {
    if (start_point != other.start_point) {
      return start_point < other.start_point;
    }
    return live_interval_idx < other.live_interval_idx;
  }
};

/*
 * Order active intervals by their first use insn idx, asc.
 */
using ActiveIntervals =
    std::priority_queue<ActiveInterval, std::vector<ActiveInterval>>;

struct LiveIntervalPoint {
  enum class Kind : uint8_t {
    MISSING,
    INSTRUCTION,
    BLOCK_END,
  };
  Kind kind = Kind::MISSING;
  union {
    IRInstruction* insn;
    cfg::BlockId block_id;
  };
  bool is_missing() const { return kind == Kind::MISSING; }
  static LiveIntervalPoint get() { return LiveIntervalPoint(); }
  static LiveIntervalPoint get(IRInstruction* insn) {
    LiveIntervalPoint lip;
    lip.kind = Kind::INSTRUCTION;
    lip.insn = insn;
    return lip;
  }
  static LiveIntervalPoint get_block_begin(cfg::Block* block) {
    auto first_insn_it = block->get_first_insn();
    return first_insn_it == block->end() ? get_block_end(block)
                                         : get(first_insn_it->insn);
  }
  static LiveIntervalPoint get_block_end(cfg::Block* block) {
    LiveIntervalPoint lip;
    lip.kind = Kind::BLOCK_END;
    lip.block_id = block->id();
    return lip;
  }
  bool operator==(const LiveIntervalPoint& other) const {
    switch (kind) {
    case Kind::MISSING:
      return other.kind == Kind::MISSING;
    case Kind::INSTRUCTION:
      return other.kind == Kind::INSTRUCTION && insn == other.insn;
    case Kind::BLOCK_END:
      return other.kind == Kind::BLOCK_END && block_id == other.block_id;
    }
  }
  struct Hasher {
    size_t operator()(const LiveIntervalPoint& lip) const {
      switch (lip.kind) {
      case Kind::MISSING:
        return 0;
      case Kind::INSTRUCTION:
        return (size_t)lip.insn;
      case Kind::BLOCK_END:
        return lip.block_id;
      }
    }
  };
};

using FreeRegPool = std::set<reg_t>;

/*
 * The shape of an instruction is defined by its opcode and possibly other fixed
 * argument. When re-using registers, we try to match end-point shapes to
 * increase the chances of creating suffices that can be deduped.
 */
struct IRInstructionShape {
  IROpcode opcode;
  union {
    // Zero-initialize this union with the uint64_t member instead of a
    // pointer-type member so that it works properly even on 32-bit machines
    uint64_t literal{0};
    const DexString* string;
    const DexType* type;
    const DexFieldRef* field;
    const DexMethodRef* method;
    const DexOpcodeData* data;
  };
  static IRInstructionShape get(const LiveIntervalPoint& lip) {
    IRInstructionShape shape;
    switch (lip.kind) {
    case LiveIntervalPoint::Kind::MISSING:
      not_reached();
    case LiveIntervalPoint::Kind::BLOCK_END:
      shape.opcode = OPCODE_NOP;
      return shape;
    case LiveIntervalPoint::Kind::INSTRUCTION: {
      auto* insn = lip.insn;
      shape.opcode = insn->opcode();
      if (insn->has_literal()) {
        shape.literal = insn->get_literal();
      } else if (insn->has_type()) {
        shape.type = insn->get_type();
      } else if (insn->has_field()) {
        shape.field = insn->get_field();
      } else if (insn->has_method()) {
        shape.method = insn->get_method();
      } else if (insn->has_string()) {
        shape.string = insn->get_string();
      } else if (insn->has_data()) {
        shape.data = insn->get_data();
      }
      return shape;
    }
    }
  }

  bool operator==(const IRInstructionShape& other) const {
    return opcode == other.opcode && literal == other.literal;
  }
};

struct IRInstructionShapeHasher {
  size_t operator()(const IRInstructionShape& shape) const {
    return shape.opcode * 27 + (size_t)shape.literal;
  }
};

/*
 * This implementation follows the pseudo algorithm proposed in paper Linear
 * Scan Register Allocation by M.Polleto and V. Sarkar
 * [https://dl.acm.org/doi/10.1145/330249.330250]
 * Except that we process the live intervals in reverse.
 */
class LinearScanAllocator final {
 public:
  explicit LinearScanAllocator(DexMethod* method);
  LinearScanAllocator(IRCode* code,
                      bool is_static,
                      const std::function<std::string()>& method_describer);

  /*
   * For each live interval in ascending order of first def:
   * expire_old_intervals; check if anything in free queue, if so, allocate a
   * free reg to the vreg in current live interval; otherwise, allocate a new
   * reg, increase cur_max_reg.
   */
  void allocate();

 private:
  // Ensure that we have an editable CFG for the duration of the optimization
  cfg::ScopedCFG m_cfg;
  bool m_is_static;

  /*
   * interval -> vreg, reg
   */
  LiveIntervals m_live_intervals;

  /*
   * vreg -> all defs 7 uses
   * Group all defs and uses of the same vreg.
   */
  VRegDefsUses m_vreg_defs_uses;

  /*
   * Record all wide vregs for allocation reference.
   */
  WideVReg m_wide_vregs;

  /*
   * We index free-regs by their last-use instruction shape, to make it more
   * likely that dedup-blocks will find matching suffixes.
   */
  UnorderedMap<IRInstructionShape, FreeRegPool, IRInstructionShapeHasher>
      m_free_regs;

  /*
   * List of live interval points indexed by live-interval start-/end-points.
   */
  std::vector<LiveIntervalPoint> m_live_interval_points;

  /*
   * We keep track of the vreg in which the "this" argument is stored. We will
   * assign it a unique register to appease the Mutator's "drop_this" function.
   */
  std::optional<reg_t> m_this_vreg;

  /*
   * { pair(interval_idx, last_use_idx) }
   * Current live intervals that has not reached their end point. i.e., live
   * intervals of active vregs.
   *
   * The reason we use "active intervals" instead of "active regs" is that
   * sorting intervals by last use idx can save work when checking for interval
   * expiration.
   */
  ActiveIntervals m_active_intervals;

  /*
   * Record the #reg allocated.
   */
  uint32_t m_reg_count = 0;

  /*
   * Find all defs and uses of each vreg by traversing the irlist.
   */
  void init_vreg_occurences();

  /*
   * Update free_regs and active_intervals: Check last use of each active
   * interval. If completed (last use < cur def), put the corresponding reg into
   * free queue; otherwise, do nothing.
   */
  void expire_old_intervals(uint32_t cur_def_idx);

  /*
   * Allocate a register for a vreg for a live-interval with the given
   * end-point. We might hand out a reused but since expired register.
   */
  reg_t allocate_register(reg_t for_vreg, uint32_t end_point);
};

} // namespace fastregalloc
