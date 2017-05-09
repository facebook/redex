/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexInstruction.h"
#include "RegisterKind.h"

using reg_t = uint16_t;

IRInstruction* gen_move(RegisterKind kind, reg_t dest, reg_t src);

/*
 * Dex opcodes vary in the maximum register slot they can address. For those
 * opcodes that can only address low regs, we insert moves before and after
 * them to shuffle their srcs / dests from high to low and back. For methods
 * that need this shuffling, we reserve a few low registers (starting from v0)
 * for this purpose.
 *
 * Opcodes that have /range equivalents are treated specially. These opcodes
 * can have up to 5 src registers, so reserving space for them at the bottom of
 * the register frame would leave fewer "unreserved" low registers and increase
 * the likelihood of inserting more move instructions. However, since these
 * have range equivalents that can address up to 16-bit register slots, we
 * instead reserve space for them at the top of the register frame (right
 * under the registers for the method arguments.)
 */
class HighRegMoveInserter {
 public:
  struct Stats {
    size_t moves_inserted {0};
    size_t range_conversions {0};
    size_t bytes_added {0};
    void add_move(IRInstruction*);
  };
  struct SwapInfo {
    // number of low registers reserved for opcodes that cannot address their
    // high register arguments
    size_t low_reg_swap {0};
    // number of high registers reserved for opcodes that we convert to /range
    // form
    size_t range_swap {0};
    SwapInfo() = default;
    SwapInfo(size_t lrs, size_t rs): low_reg_swap(lrs), range_swap(rs) {}
    bool operator==(const SwapInfo& that) const {
      return low_reg_swap == that.low_reg_swap && range_swap == that.range_swap;
    }
  };

  static SwapInfo reserve_swap(DexMethod* method);
  void insert_moves(IRCode*, const SwapInfo&);
  const Stats& get_stats() const { return m_stats; }
 private:
  void handle_rangeable(IRCode* mt,
                        InstructionIterator& it,
                        const KindVec& reg_kinds,
                        reg_t range_start);
  static size_t low_reg_space_needed(IRCode* code);
  static size_t range_space_needed(IRCode* code);
  static void increment_all_regs(IRCode*, size_t size);

  Stats m_stats;
};

class RegAllocPass : public Pass {
public:
  RegAllocPass() : Pass("RegAllocPass") {}
  virtual void configure_pass(const PassConfig&) override {}
  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
