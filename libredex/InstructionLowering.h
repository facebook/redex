/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "IRCode.h"
#include "IRInstruction.h"
#include "Pass.h"

namespace instruction_lowering {

struct Stats {
  size_t to_2addr{0};
  size_t move_for_check_cast{0};
  void accumulate(const Stats& that) {
    to_2addr += that.to_2addr;
    move_for_check_cast += that.move_for_check_cast;
  }
};

/*
 * Convert IRInstructions to DexInstructions while doing the following:
 *
 *   - Check consistency of load-param opcodes
 *   - Pick the smallest opcode that can address its operands.
 *   - Insert move instructions as necessary for check-cast instructions that
 *     have different src and dest registers.
 *   - Record the number of instructions converted to /2ddr form, also the
 *     number of move instruction inserted because of check-cast.
 */
Stats lower(DexMethod*);

Stats run(DexStoresVector&);

namespace impl {

DexOpcode select_move_opcode(const IRInstruction* insn);

DexOpcode select_const_opcode(const IRInstruction* insn);

DexOpcode select_binop_lit_opcode(const IRInstruction* insn);

bool try_2addr_conversion(MethodItemEntry*);

} // namespace impl

} // namespace select_instructions
