/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
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
Stats lower(DexMethod*, bool lower_with_cfg = false);

Stats run(DexStoresVector&, bool lower_with_cfg = false);

namespace impl {

DexOpcode select_move_opcode(const IRInstruction* insn);

DexOpcode select_const_opcode(const IRInstruction* insn);

DexOpcode select_binop_lit_opcode(const IRInstruction* insn);

bool try_2addr_conversion(MethodItemEntry*);

} // namespace impl

// Computes number of entries needed for a packed switch, accounting for any
// holes that might exist
uint64_t get_packed_switch_size(const std::vector<int32_t>& case_keys);

// Whether a sparse switch statement will be more compact than a packed switch
bool sufficiently_sparse(const std::vector<int32_t>& case_keys);

} // namespace instruction_lowering
