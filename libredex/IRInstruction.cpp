/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "IRInstruction.h"

namespace {

bool can_use_2addr(const IRInstruction* insn) {
  auto op = insn->opcode();
  return op >= OPCODE_ADD_INT && op <= OPCODE_REM_DOUBLE &&
         insn->dest() == insn->src(0) && insn->dest() <= 0xf &&
         insn->src(1) <= 0xf;
}

DexOpcode convert_2to3addr(DexOpcode op) {
  always_assert(op >= OPCODE_ADD_INT_2ADDR && op <= OPCODE_REM_DOUBLE_2ADDR);
  constexpr uint16_t offset = OPCODE_ADD_INT_2ADDR - OPCODE_ADD_INT;
  return (DexOpcode)(op - offset);
}

DexOpcode convert_3to2addr(DexOpcode op) {
  always_assert(op >= OPCODE_ADD_INT && op <= OPCODE_REM_DOUBLE);
  constexpr uint16_t offset = OPCODE_ADD_INT_2ADDR - OPCODE_ADD_INT;
  return (DexOpcode)(op + offset);
}

}

IRInstruction* IRInstruction::make(const DexInstruction* insn) {
  IRInstruction* ir_insn;
  if (insn->has_strings()) {
    ir_insn =
        new IRStringInstruction(static_cast<const DexOpcodeString*>(insn));
  } else if (insn->has_types()) {
    ir_insn = new IRTypeInstruction(static_cast<const DexOpcodeType*>(insn));
  } else if (insn->has_fields()) {
    ir_insn = new IRFieldInstruction(static_cast<const DexOpcodeField*>(insn));
  } else if (insn->has_methods()) {
    ir_insn =
        new IRMethodInstruction(static_cast<const DexOpcodeMethod*>(insn));
  } else {
    ir_insn = new IRInstruction(insn);
  }
  return ir_insn;
}

IRInstruction::IRInstruction(DexOpcode op) : Gatherable(), m_opcode(op) {
  always_assert(!is_fopcode(op));
  m_srcs.resize(opcode::min_srcs_size(op));
}

IRInstruction::IRInstruction(const DexInstruction* insn) : Gatherable() {
  m_opcode = insn->opcode();
  always_assert(!is_fopcode(m_opcode));
  if (opcode::dests_size(m_opcode)) {
    m_dest = insn->dest();
  }
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    m_srcs.emplace_back(insn->src(i));
  }
  if (opcode::dest_is_src(m_opcode)) {
    m_opcode = convert_2to3addr(m_opcode);
  }
  if (opcode::has_literal(m_opcode)) {
    m_literal = insn->literal();
  }
  if (opcode::has_offset(m_opcode)) {
    m_offset = insn->offset();
  }
  if (opcode::has_range(m_opcode)) {
    m_range =
        std::pair<uint16_t, uint16_t>(insn->range_base(), insn->range_size());
  }
}

bool IRInstruction::operator==(const IRInstruction& that) const {
  return m_ref_type == that.m_ref_type &&
    m_opcode == that.m_opcode &&
    m_srcs == that.m_srcs &&
    m_dest == that.m_dest &&
    m_literal == that.m_literal &&
    m_offset == that.m_offset &&
    m_range == that.m_range;
}

uint16_t IRInstruction::size() const {
  static int args[] = {
      0, /* FMT_f00x   */
      1, /* FMT_f10x   */
      1, /* FMT_f12x   */
      1, /* FMT_f12x_2 */
      1, /* FMT_f11n   */
      1, /* FMT_f11x_d */
      1, /* FMT_f11x_s */
      1, /* FMT_f10t   */
      2, /* FMT_f20t   */
      2, /* FMT_f20bc  */
      2, /* FMT_f22x   */
      2, /* FMT_f21t   */
      2, /* FMT_f21s   */
      2, /* FMT_f21h   */
      2, /* FMT_f21c_d */
      2, /* FMT_f21c_s */
      2, /* FMT_f23x_d */
      2, /* FMT_f23x_s */
      2, /* FMT_f22b   */
      2, /* FMT_f22t   */
      2, /* FMT_f22s   */
      2, /* FMT_f22c_d */
      2, /* FMT_f22c_s */
      2, /* FMT_f22cs  */
      3, /* FMT_f30t   */
      3, /* FMT_f32x   */
      3, /* FMT_f31i   */
      3, /* FMT_f31t   */
      3, /* FMT_f31c   */
      3, /* FMT_f35c   */
      3, /* FMT_f35ms  */
      3, /* FMT_f35mi  */
      3, /* FMT_f3rc   */
      3, /* FMT_f3rms  */
      3, /* FMT_f3rmi  */
      5, /* FMT_f51l   */
      4, /* FMT_f41c_d */
      4, /* FMT_f41c_s */
      5, /* FMT_f52c_d */
      5, /* FMT_f52c_s */
      5, /* FMT_f5rc */
      5, /* FMT_f57c */
      0, /* FMT_fopcode   */
  };
  auto op = can_use_2addr(this) ? convert_3to2addr(opcode()) : opcode();
  return args[opcode::format(op)];
}

void IRInstruction::set_dex_instruction_args(DexInstruction* insn) const {
  if (insn->dests_size()) {
    insn->set_dest(dest());
  }
  for (size_t i = 0; i < srcs_size(); ++i) {
    insn->set_src(i, src(i));
  }
  if (opcode::has_literal(insn->opcode())) {
    insn->set_literal(literal());
  }
  if (opcode::has_offset(insn->opcode())) {
    insn->set_offset(offset());
  }
  if (insn->has_arg_word_count()) {
    insn->set_arg_word_count(srcs_size());
  }
  if (opcode::has_range(insn->opcode())) {
    insn->set_range_base(range_base());
    insn->set_range_size(range_size());
  }
}

DexInstruction* IRInstruction::to_dex_instruction() const {
  auto op = can_use_2addr(this) ? convert_3to2addr(opcode()) : opcode();
  auto insn = new DexInstruction(op);
  set_dex_instruction_args(insn);
  return insn;
}

DexInstruction* IRStringInstruction::to_dex_instruction() const {
  auto insn = new DexOpcodeString(opcode(), m_string);
  set_dex_instruction_args(insn);
  return insn;
}

DexInstruction* IRTypeInstruction::to_dex_instruction() const {
  auto insn = new DexOpcodeType(opcode(), m_type);
  set_dex_instruction_args(insn);
  return insn;
}

DexInstruction* IRFieldInstruction::to_dex_instruction() const {
  auto insn = new DexOpcodeField(opcode(), m_field);
  set_dex_instruction_args(insn);
  return insn;
}

DexInstruction* IRMethodInstruction::to_dex_instruction() const {
  auto insn = new DexOpcodeMethod(opcode(), m_method);
  set_dex_instruction_args(insn);
  return insn;
}

bool IRInstruction::dest_is_wide() const {
  switch (opcode()) {
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_WIDE_FROM16:
  case OPCODE_MOVE_WIDE_16:
  case OPCODE_MOVE_RESULT_WIDE:
    return true;

  case OPCODE_CONST_WIDE_16:
  case OPCODE_CONST_WIDE_32:
  case OPCODE_CONST_WIDE:
  case OPCODE_CONST_WIDE_HIGH16:
    return true;

  case OPCODE_AGET_WIDE:
  case OPCODE_IGET_WIDE:
  case OPCODE_SGET_WIDE:
    return true;

  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG:
  case OPCODE_NEG_DOUBLE:
  case OPCODE_INT_TO_LONG:
  case OPCODE_INT_TO_DOUBLE:
  case OPCODE_LONG_TO_DOUBLE:
  case OPCODE_FLOAT_TO_LONG:
  case OPCODE_FLOAT_TO_DOUBLE:
  case OPCODE_DOUBLE_TO_LONG:
    return true;

  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG:
  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG:
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE:
  case OPCODE_ADD_LONG_2ADDR:
  case OPCODE_SUB_LONG_2ADDR:
  case OPCODE_MUL_LONG_2ADDR:
  case OPCODE_DIV_LONG_2ADDR:
  case OPCODE_REM_LONG_2ADDR:
  case OPCODE_AND_LONG_2ADDR:
  case OPCODE_OR_LONG_2ADDR:
  case OPCODE_XOR_LONG_2ADDR:
  case OPCODE_SHL_LONG_2ADDR:
  case OPCODE_SHR_LONG_2ADDR:
  case OPCODE_USHR_LONG_2ADDR:
  case OPCODE_ADD_DOUBLE_2ADDR:
  case OPCODE_SUB_DOUBLE_2ADDR:
  case OPCODE_MUL_DOUBLE_2ADDR:
  case OPCODE_DIV_DOUBLE_2ADDR:
  case OPCODE_REM_DOUBLE_2ADDR:
    return true;

  default:
    return false;
  }
}

bool IRInstruction::src_is_wide(size_t i) const {
  switch (opcode()) {
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_WIDE_FROM16:
  case OPCODE_MOVE_WIDE_16:
  case OPCODE_RETURN_WIDE:
    return i == 0;

  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE:
  case OPCODE_CMP_LONG:
    return i == 0 || i == 1;

  case OPCODE_APUT_WIDE:
  case OPCODE_IPUT_WIDE:
  case OPCODE_SPUT_WIDE:
    return i == 0;

  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG:
  case OPCODE_NEG_DOUBLE:
  case OPCODE_LONG_TO_INT:
  case OPCODE_LONG_TO_FLOAT:
  case OPCODE_LONG_TO_DOUBLE:
  case OPCODE_DOUBLE_TO_INT:
  case OPCODE_DOUBLE_TO_LONG:
  case OPCODE_DOUBLE_TO_FLOAT:
    return i == 0;

  case OPCODE_ADD_LONG:
  case OPCODE_SUB_LONG:
  case OPCODE_MUL_LONG:
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG:
  case OPCODE_AND_LONG:
  case OPCODE_OR_LONG:
  case OPCODE_XOR_LONG:
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE:
  case OPCODE_ADD_LONG_2ADDR:
  case OPCODE_SUB_LONG_2ADDR:
  case OPCODE_MUL_LONG_2ADDR:
  case OPCODE_DIV_LONG_2ADDR:
  case OPCODE_REM_LONG_2ADDR:
  case OPCODE_AND_LONG_2ADDR:
  case OPCODE_OR_LONG_2ADDR:
  case OPCODE_XOR_LONG_2ADDR:
  case OPCODE_ADD_DOUBLE_2ADDR:
  case OPCODE_SUB_DOUBLE_2ADDR:
  case OPCODE_MUL_DOUBLE_2ADDR:
  case OPCODE_DIV_DOUBLE_2ADDR:
  case OPCODE_REM_DOUBLE_2ADDR:
    return i == 0 || i == 1;

  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG:
  case OPCODE_SHL_LONG_2ADDR:
  case OPCODE_SHR_LONG_2ADDR:
  case OPCODE_USHR_LONG_2ADDR:
    return i == 0;

  default:
    return false;
  }
}
