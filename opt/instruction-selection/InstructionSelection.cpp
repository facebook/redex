/**
 * Copyright (c) 2017-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "InstructionSelection.h"
#include "ParallelWalkers.h"

namespace select_instructions {

/*
 * Returns whether the given value can fit in an integer of :width bits.
 */
template <int width>
static bool signed_int_fits(int64_t v) {
  auto shift = 64 - width;
  return (v << shift >> shift) == v;
}

/*
 * Returns whether the given value's significant bits can fit in the top 16
 * bits of an integer of :total_width bits. For example, since v is a signed
 * 64-bit int, a value v that can fit into the top 16 bits of a 32-bit int
 * would have the form 0xffffffffrrrr0000, where rrrr are the significant bits.
 */
template <int total_width>
static bool signed_int_fits_high16(int64_t v) {
  auto right_zeros = total_width - 16;
  auto left_ones = 64 - total_width;
  return v >> right_zeros << (64 - 16) >> left_ones == v;
}

/*
 * Helpers for select_instructions
 */

/*
 * Returns an array of move opcodes of the appropriate type, sorted by
 * increasing size.
 */
static std::array<DexOpcode, 3> move_opcode_tuple(DexOpcode op) {
  switch (op) {
  case OPCODE_MOVE:
  case OPCODE_MOVE_FROM16:
  case OPCODE_MOVE_16:
    return {{OPCODE_MOVE, OPCODE_MOVE_FROM16, OPCODE_MOVE_16}};
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_WIDE_FROM16:
  case OPCODE_MOVE_WIDE_16:
    return {{OPCODE_MOVE_WIDE, OPCODE_MOVE_WIDE_FROM16, OPCODE_MOVE_WIDE_16}};
  case OPCODE_MOVE_OBJECT:
  case OPCODE_MOVE_OBJECT_FROM16:
  case OPCODE_MOVE_OBJECT_16:
    return {
        {OPCODE_MOVE_OBJECT, OPCODE_MOVE_OBJECT_FROM16, OPCODE_MOVE_OBJECT_16}};
  default:
    not_reached();
  }
}

DexOpcode select_move_opcode(const IRInstruction* insn) {
  auto move_tuple = move_opcode_tuple(insn->opcode());
  auto dest_width = required_bit_width(insn->dest());
  auto src_width = required_bit_width(insn->src(0));
  if (dest_width <= 4 && src_width <= 4) {
    return move_tuple.at(0);
  } else if (dest_width <= 8) {
    return move_tuple.at(1);
  } else {
    return move_tuple.at(2);
  }
}

DexOpcode select_const_opcode(const IRInstruction* insn) {
  auto op = insn->opcode();
  auto dest_width = required_bit_width(insn->dest());
  always_assert(dest_width <= 8);
  auto literal = insn->literal();
  switch (op) {
  case OPCODE_CONST_4:
  case OPCODE_CONST_16:
  case OPCODE_CONST_HIGH16:
  case OPCODE_CONST:
    if (dest_width <= 4 && signed_int_fits<4>(literal)) {
      return OPCODE_CONST_4;
    } else if (signed_int_fits<16>(literal)) {
      return OPCODE_CONST_16;
    } else if (signed_int_fits_high16<32>(literal)) {
      return OPCODE_CONST_HIGH16;
    } else {
      return OPCODE_CONST;
    }
  case OPCODE_CONST_WIDE_16:
  case OPCODE_CONST_WIDE_32:
  case OPCODE_CONST_WIDE_HIGH16:
  case OPCODE_CONST_WIDE:
    if (signed_int_fits<16>(literal)) {
      return OPCODE_CONST_WIDE_16;
    } else if (signed_int_fits<32>(literal)) {
      return OPCODE_CONST_WIDE_32;
    } else if (signed_int_fits_high16<64>(literal)) {
      return OPCODE_CONST_WIDE_HIGH16;
    } else {
      return OPCODE_CONST_WIDE;
    }
  default:
    not_reached();
  }
}

void InstructionSelection::select_instructions(IRCode* code) {
  auto ii = InstructionIterable(code);
  auto end = ii.end();
  for (auto it = ii.begin(); it != end; ++it) {
    auto* insn = it->insn;
    auto op = insn->opcode();
    m_stats.to_2addr += try_2addr_conversion(insn);
    if (op == OPCODE_CHECK_CAST && insn->dest() != insn->src(0)) {
      // convert check-cast v0, v1 into
      //
      //   move v0, v1
      //   check-cast v0
      auto* mov = new IRInstruction(OPCODE_MOVE_OBJECT_16);
      mov->set_dest(insn->dest());
      mov->set_src(0, insn->src(0));
      mov->set_opcode(select_move_opcode(mov));
      insn->set_src(0, insn->dest());
      code->insert_before(it.unwrap(), mov);
      ++m_stats.move_for_check_cast;
    } else if (is_move(op)) {
      insn->set_opcode(select_move_opcode(insn));
    } else if (op >= OPCODE_CONST_4 && op <= OPCODE_CONST_WIDE) {
      insn->set_opcode(select_const_opcode(insn));
    }
    // TODO: /lit8 and /lit16 instructions
  }
}

bool is_commutative(DexOpcode op) {
  return op == OPCODE_ADD_INT || op == OPCODE_MUL_INT ||
         (op >= OPCODE_AND_INT && op <= OPCODE_XOR_INT) ||
         op == OPCODE_ADD_LONG || op == OPCODE_MUL_LONG ||
         (op >= OPCODE_AND_LONG && op <= OPCODE_XOR_LONG) ||
         op == OPCODE_ADD_FLOAT || op == OPCODE_MUL_FLOAT ||
         op == OPCODE_ADD_DOUBLE || op == OPCODE_MUL_DOUBLE;
}

bool try_2addr_conversion(IRInstruction* insn) {
  auto op = insn->opcode();
  if (is_commutative(op) && insn->dest() == insn->src(1) &&
      insn->dest() <= 0xf && insn->src(0) <= 0xf) {
    uint16_t reg_temp = insn->src(0);
    insn->set_src(0, insn->src(1));
    insn->set_src(1, reg_temp);
    insn->set_opcode(convert_3to2addr(op));
    return true;
  } else if (op >= OPCODE_ADD_INT && op <= OPCODE_REM_DOUBLE &&
             insn->dest() == insn->src(0) && insn->dest() <= 0xf &&
             insn->src(1) <= 0xf) {
    insn->set_opcode(convert_3to2addr(op));
    return true;
  }
  return false;
}

} // namespace select_instructions

using namespace select_instructions;

void InstructionSelectionPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles&,
                                        PassManager& mgr) {
  using Data = std::nullptr_t;
  using Output = InstructionSelection::Stats;
  auto scope = build_class_scope(stores);
  auto mapper = [](Data&, DexMethod* m) {
    InstructionSelection::Stats stats;
    if (m->get_code() == nullptr) {
      return stats;
    }
    auto code = m->get_code();
    InstructionSelection select;
    select.select_instructions(code);
    stats.accumulate(select.get_stats());
    return stats;
  };
  auto stats = walk_methods_parallel<Scope, Data, Output>(
      scope,
      mapper,
      [](Output a, Output b) {
        a.accumulate(b);
        return a;
      },
      [&](unsigned int) { return nullptr; });
  mgr.incr_metric("num_instruction_to_2addr", stats.to_2addr);
  mgr.incr_metric("num_move_added_for_check_cast", stats.move_for_check_cast);
}

static InstructionSelectionPass s_pass;
