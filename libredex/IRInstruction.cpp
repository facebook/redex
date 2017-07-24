/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "IRInstruction.h"

#include "DexClass.h"
#include "DexUtil.h"

namespace {

DexOpcode opcode_no_range_version(DexOpcode op) {
  switch (op) {
  case OPCODE_INVOKE_DIRECT_RANGE:
    return OPCODE_INVOKE_DIRECT;
  case OPCODE_INVOKE_STATIC_RANGE:
    return OPCODE_INVOKE_STATIC;
  case OPCODE_INVOKE_SUPER_RANGE:
    return OPCODE_INVOKE_SUPER;
  case OPCODE_INVOKE_VIRTUAL_RANGE:
    return OPCODE_INVOKE_VIRTUAL;
  case OPCODE_INVOKE_INTERFACE_RANGE:
    return OPCODE_INVOKE_INTERFACE;
  case OPCODE_FILLED_NEW_ARRAY_RANGE:
    return OPCODE_FILLED_NEW_ARRAY;
  default:
    always_assert(false);
  }
}

DexOpcode opcode_range_version(DexOpcode op) {
  switch (op) {
  case OPCODE_INVOKE_DIRECT:
    return OPCODE_INVOKE_DIRECT_RANGE;
  case OPCODE_INVOKE_STATIC:
    return OPCODE_INVOKE_STATIC_RANGE;
  case OPCODE_INVOKE_SUPER:
    return OPCODE_INVOKE_SUPER_RANGE;
  case OPCODE_INVOKE_VIRTUAL:
    return OPCODE_INVOKE_VIRTUAL_RANGE;
  case OPCODE_INVOKE_INTERFACE:
    return OPCODE_INVOKE_INTERFACE_RANGE;
  case OPCODE_FILLED_NEW_ARRAY:
    return OPCODE_FILLED_NEW_ARRAY_RANGE;
  default:
    always_assert(false);
  }
}

} // namespace

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

IRInstruction::IRInstruction(DexOpcode op) : m_opcode(op) {
  always_assert(!is_fopcode(op));
  m_srcs.resize(opcode_impl::min_srcs_size(op));
}

IRInstruction::IRInstruction(const DexInstruction* insn) {
  m_opcode = insn->opcode();
  always_assert(!is_fopcode(m_opcode));
  if (opcode_impl::dests_size(m_opcode)) {
    m_dest = insn->dest();
  } else if (m_opcode == OPCODE_CHECK_CAST) {
    m_dest = insn->src(0);
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
  if (insn->has_string()) {
    set_string(static_cast<const DexOpcodeString*>(insn)->get_string());
  } else if (insn->has_type()) {
    set_type(static_cast<const DexOpcodeType*>(insn)->get_type());
  } else if (insn->has_field()) {
    set_field(static_cast<const DexOpcodeField*>(insn)->get_field());
  } else if (insn->has_method()) {
    set_method(static_cast<const DexOpcodeMethod*>(insn)->get_method());
  }
}

// Structural equality of opcodes except branches offsets are ignored
// because they are unknown until we sync back to DexInstructions.
bool IRInstruction::operator==(const IRInstruction& that) const {
  return m_opcode == that.m_opcode &&
    m_string == that.m_string && // just test one member of the union
    m_srcs == that.m_srcs &&
    m_dest == that.m_dest &&
    m_literal == that.m_literal &&
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
      0, /* FMT_iopcode   */
  };
  return args[opcode::format(opcode())];
}

DexInstruction* IRInstruction::to_dex_instruction() const {
  DexInstruction* insn;
  switch (opcode::ref(opcode())) {
    case opcode::Ref::None:
    case opcode::Ref::Data:
      insn = new DexInstruction(opcode());
      break;
    case opcode::Ref::String:
      insn = new DexOpcodeString(opcode(), m_string);
      break;
    case opcode::Ref::Type:
      insn = new DexOpcodeType(opcode(), m_type);
      break;
    case opcode::Ref::Field:
      insn = new DexOpcodeField(opcode(), m_field);
      break;
    case opcode::Ref::Method:
      insn = new DexOpcodeMethod(opcode(), m_method);
      break;
  }

  if (opcode() == OPCODE_CHECK_CAST || opcode::dest_is_src(opcode())) {
    always_assert(dest() == src(0));
  } else if (insn->dests_size()) {
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

  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
    return false;
  case IOPCODE_LOAD_PARAM_WIDE:
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

bit_width_t IRInstruction::dest_bit_width() const {
  if (m_opcode == OPCODE_CHECK_CAST) {
    return 4;
  } else {
    return opcode_impl::dest_bit_width(m_opcode);
  }
}

bit_width_t IRInstruction::src_bit_width(uint16_t i) const {
  return opcode_impl::src_bit_width(m_opcode, i);
}

void IRInstruction::normalize_registers() {
  if (is_invoke(opcode()) && !opcode::has_range(opcode())) {
    auto& args = get_method()->get_proto()->get_args()->get_type_list();
    size_t old_srcs_idx {0};
    size_t srcs_idx {0};
    if (m_opcode != OPCODE_INVOKE_STATIC) {
      ++srcs_idx;
      ++old_srcs_idx;
    }
    for (size_t args_idx = 0; args_idx < args.size(); ++args_idx) {
      set_src(srcs_idx++, src(old_srcs_idx));
      old_srcs_idx += is_wide_type(args.at(args_idx)) ? 2 : 1;
    }
    set_arg_word_count(srcs_idx);
  }
}

void IRInstruction::denormalize_registers() {
  always_assert(!opcode::has_range(m_opcode));
  if (is_invoke(m_opcode)) {
    auto& args = get_method()->get_proto()->get_args()->get_type_list();
    std::vector<uint16_t> srcs;
    size_t args_idx {0};
    size_t srcs_idx {0};
    if (m_opcode != OPCODE_INVOKE_STATIC) {
      srcs.push_back(src(srcs_idx++));
    }
    bool has_wide {false};
    for (; args_idx < args.size(); ++args_idx, ++srcs_idx) {
      srcs.push_back(src(srcs_idx));
      if (is_wide_type(args.at(args_idx))) {
        srcs.push_back(src(srcs_idx) + 1);
        has_wide = true;
      }
    }
    if (has_wide) {
      m_srcs = srcs;
    }
  }
}

void IRInstruction::range_to_srcs() {
  if (!opcode::has_range(m_opcode)) {
    return;
  }
  set_arg_word_count(range_size());
  for (size_t i = 0; i < range_size(); ++i) {
    set_src(i, range_base() + i);
  }
  set_range_base(0);
  set_range_size(0);
  m_opcode = opcode_no_range_version(m_opcode);
}

bit_width_t required_bit_width(uint16_t v) {
  bit_width_t result {1};
  while (v >>= 1) {
    ++result;
  }
  return result;
}

bool has_contiguous_srcs(const IRInstruction* insn) {
  if (insn->srcs_size() == 0) {
    return true;
  }
  auto last = insn->src(0);
  for (size_t i = 1; i < insn->srcs_size(); ++i) {
    if (insn->src(i) - last != 1) {
      return false;
    }
    last = insn->src(i);
  }
  return true;
}

bool needs_range_conversion(const IRInstruction* insn) {
  auto op = insn->opcode();
  always_assert(opcode::has_range_form(op));
  if (insn->srcs_size() > opcode::NON_RANGE_MAX) {
    return true;
  }
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    if (required_bit_width(insn->src(i)) > insn->src_bit_width(i)) {
      return true;
    }
  }
  return false;
}

void IRInstruction::srcs_to_range() {
  if (!opcode::has_range_form(m_opcode) || !needs_range_conversion(this)) {
    return;
  }
  // the register allocator is responsible for ensuring that this property is
  // true
  always_assert(has_contiguous_srcs(this));
  m_opcode = opcode_range_version(m_opcode);
  set_range_base(src(0));
  set_range_size(srcs_size());
  set_arg_word_count(0);
}
