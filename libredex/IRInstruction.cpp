/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IRInstruction.h"

#include "DexClass.h"
#include "DexUtil.h"

DexOpcode convert_2to3addr(DexOpcode op) {
  always_assert(op >= DOPCODE_ADD_INT_2ADDR && op <= DOPCODE_REM_DOUBLE_2ADDR);
  constexpr uint16_t offset = DOPCODE_ADD_INT_2ADDR - DOPCODE_ADD_INT;
  return (DexOpcode)(op - offset);
}

DexOpcode convert_3to2addr(DexOpcode op) {
  always_assert(op >= DOPCODE_ADD_INT && op <= DOPCODE_REM_DOUBLE);
  constexpr uint16_t offset = DOPCODE_ADD_INT_2ADDR - DOPCODE_ADD_INT;
  return (DexOpcode)(op + offset);
}

IRInstruction::IRInstruction(IROpcode op) : m_opcode(op) {
  m_srcs.resize(opcode_impl::min_srcs_size(op));
}

// Structural equality of opcodes except branches offsets are ignored
// because they are unknown until we sync back to DexInstructions.
bool IRInstruction::operator==(const IRInstruction& that) const {
  return m_opcode == that.m_opcode &&
    m_string == that.m_string && // just test one member of the union
    m_srcs == that.m_srcs &&
    m_dest == that.m_dest &&
    m_literal == that.m_literal;
}

uint16_t IRInstruction::size() const {
  auto op = m_opcode;
  if (opcode::is_internal(op)) {
    return 0;
  }
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
  return args[dex_opcode::format(opcode::to_dex_opcode(op))];
}

// The instruction format doesn't tell us the width of registers for invoke
// so we inspect the signature of the method we're calling
bool IRInstruction::invoke_src_is_wide(size_t i) const {
  always_assert(has_method());

  // virtual methods have `this` as the 0th register argument, but the
  // arg list does NOT include `this`
  if (!is_invoke_static(m_opcode)) {
    if (i == 0) {
      // reference to `this`. References are never wide
      return false;
    }
    --i;
  }

  const std::deque<DexType*>& args =
      m_method->get_proto()->get_args()->get_type_list();
  return is_wide_type(args[i]);
}

bool IRInstruction::src_is_wide(size_t i) const {
  always_assert(i < srcs_size());

  if (is_invoke(m_opcode)) {
    return invoke_src_is_wide(i);
  }

  switch (opcode()) {
  case OPCODE_MOVE_WIDE:
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
    return i == 0 || i == 1;

  case OPCODE_SHL_LONG:
  case OPCODE_SHR_LONG:
  case OPCODE_USHR_LONG:
    return i == 0;

  default:
    return false;
  }
}

void IRInstruction::normalize_registers() {
  if (is_invoke(opcode())) {
    auto& args = get_method()->get_proto()->get_args()->get_type_list();
    size_t old_srcs_idx{0};
    size_t srcs_idx{0};
    if (m_opcode != OPCODE_INVOKE_STATIC) {
      ++srcs_idx;
      ++old_srcs_idx;
    }
    for (size_t args_idx = 0; args_idx < args.size(); ++args_idx) {
      always_assert_log(
          old_srcs_idx < srcs_size(),
          "Invalid arg indices in %s args_idx %d old_srcs_idx %d\n",
          SHOW(this),
          args_idx,
          old_srcs_idx);
      set_src(srcs_idx++, src(old_srcs_idx));
      old_srcs_idx += is_wide_type(args.at(args_idx)) ? 2 : 1;
    }
    always_assert(old_srcs_idx == srcs_size());
    set_arg_word_count(srcs_idx);
  }
}

void IRInstruction::denormalize_registers() {
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
  if (!opcode::has_range_form(op)) {
    return false;
  }
  if (insn->srcs_size() > dex_opcode::NON_RANGE_MAX) {
    return true;
  }
  always_assert(!opcode::is_internal(op));
  auto dex_op = opcode::to_dex_opcode(op);
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    if (required_bit_width(insn->src(i)) >
        dex_opcode::src_bit_width(dex_op, i)) {
      return true;
    }
  }
  return false;
}

uint64_t IRInstruction::hash() const {
  std::vector<uint64_t> bits;
  bits.push_back(opcode());

  for (size_t i = 0; i < srcs_size(); i++) {
    bits.push_back(src(i));
  }

  if (dests_size() > 0) {
    bits.push_back(dest());
  }

  if (has_data()) {
    size_t size = get_data()->data_size();
    const auto& data = get_data()->data();
    for (size_t i = 0; i < size; i++) {
      bits.push_back(data[i]);
    }
  }

  if (has_type()) {
    bits.push_back(reinterpret_cast<uint64_t>(get_type()));
  }
  if (has_field()) {
    bits.push_back(reinterpret_cast<uint64_t>(get_field()));
  }
  if (has_method()) {
    bits.push_back(reinterpret_cast<uint64_t>(get_method()));
  }
  if (has_string()) {
    bits.push_back(reinterpret_cast<uint64_t>(get_string()));
  }
  if (has_literal()) {
    bits.push_back(get_literal());
  }

  uint64_t result = 0;
  for (uint64_t elem : bits) {
    result ^= elem;
  }
  return result;
}
