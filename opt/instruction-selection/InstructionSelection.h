/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "IRInstruction.h"
#include "Pass.h"
#include "Transform.h"

namespace select_instructions {

class InstructionSelection {
 public:
  struct Stats {
    size_t to_2addr{0};
    size_t move_for_check_cast{0};
    void accumulate(const Stats& that) {
      to_2addr += that.to_2addr;
      move_for_check_cast += that.move_for_check_cast;
    }
  };

  /*
   * Pick the smallest opcode that can address its operands.
   *
   * Also insert move instructions as necessary for check-cast instructions that
   * have different src and dest registers.
   *
   * Record the number of instructions converted to /2ddr form, also the number
   * of move instruction inserted because of check-cast.
   */
  void select_instructions(IRCode* code);

  const Stats& get_stats() const { return m_stats; }

 private:
  Stats m_stats;
};

static std::array<DexOpcode, 3> move_opcode_tuple(DexOpcode op);

DexOpcode select_move_opcode(const IRInstruction* insn);

DexOpcode select_const_opcode(const IRInstruction* insn);

bool is_commutative(DexOpcode op);

bool try_2addr_conversion(IRInstruction*);

} // namespace select_instructions

class InstructionSelectionPass : public Pass {
 public:
  InstructionSelectionPass() : Pass("InstructionSelectionPass") {}

  virtual void run_pass(DexStoresVector&, ConfigFiles&, PassManager&) override;
};
