/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexOpcode.h"

#include "Debug.h"
#include "Show.h"

namespace dex_opcode {

OpcodeFormat format(DexOpcode opcode) {
  switch (opcode) {
#define OP(op, code, fmt, ...) \
  case code:                   \
    return FMT_##fmt;
    DOPS
#undef OP
  case FOPCODE_PACKED_SWITCH :
    return FMT_fopcode;
  case FOPCODE_SPARSE_SWITCH:
    return FMT_fopcode;
  case FOPCODE_FILLED_ARRAY:
    return FMT_fopcode;
#define OP(op, code, fmt, ...) \
  case code:                   \
    always_assert_log(false, "Unexpected quick opcode 0x%x", opcode);
    break;
    QDOPS
#undef OP
  }
  always_assert_log(false, "Unexpected opcode 0x%x", opcode);
}

bool dest_is_src(DexOpcode op) {
  return format(op) == FMT_f12x_2;
}

bool has_literal(DexOpcode op) {
  auto fmt = format(op);
  switch (fmt) {
  case FMT_f11n:
  case FMT_f21s:
  case FMT_f21h:
  case FMT_f22b:
  case FMT_f22s:
  case FMT_f31i:
  case FMT_f51l:
    return true;
  default:
    return false;
  }
  not_reached();
}

bool has_offset(DexOpcode op) {
  switch (format(op)) {
  case FMT_f10t:
  case FMT_f20t:
  case FMT_f21t:
  case FMT_f22t:
  case FMT_f30t:
  case FMT_f31t:
    return true;
  default:
    return false;
  }
  not_reached();
}

bool has_range(DexOpcode op) {
  auto fmt = format(op);
  if (fmt == FMT_f3rc || fmt == FMT_f5rc)
    return true;
  return false;
}

bool is_commutative(DexOpcode op) {
  return op == DOPCODE_ADD_INT || op == DOPCODE_MUL_INT ||
         (op >= DOPCODE_AND_INT && op <= DOPCODE_XOR_INT) ||
         op == DOPCODE_ADD_LONG || op == DOPCODE_MUL_LONG ||
         (op >= DOPCODE_AND_LONG && op <= DOPCODE_XOR_LONG) ||
         op == DOPCODE_ADD_FLOAT || op == DOPCODE_MUL_FLOAT ||
         op == DOPCODE_ADD_DOUBLE || op == DOPCODE_MUL_DOUBLE;
}

bool is_branch(DexOpcode op) {
  switch (op) {
  case DOPCODE_PACKED_SWITCH:
  case DOPCODE_SPARSE_SWITCH:
  case DOPCODE_GOTO_32:
  case DOPCODE_IF_EQ:
  case DOPCODE_IF_NE:
  case DOPCODE_IF_LT:
  case DOPCODE_IF_GE:
  case DOPCODE_IF_GT:
  case DOPCODE_IF_LE:
  case DOPCODE_IF_EQZ:
  case DOPCODE_IF_NEZ:
  case DOPCODE_IF_LTZ:
  case DOPCODE_IF_GEZ:
  case DOPCODE_IF_GTZ:
  case DOPCODE_IF_LEZ:
  case DOPCODE_GOTO_16:
  case DOPCODE_GOTO:
    return true;
  default:
    return false;
  }
}

bool is_conditional_branch(DexOpcode op) {
  switch (op) {
  case DOPCODE_IF_EQ:
  case DOPCODE_IF_NE:
  case DOPCODE_IF_LT:
  case DOPCODE_IF_GE:
  case DOPCODE_IF_GT:
  case DOPCODE_IF_LE:
  case DOPCODE_IF_EQZ:
  case DOPCODE_IF_NEZ:
  case DOPCODE_IF_LTZ:
  case DOPCODE_IF_GEZ:
  case DOPCODE_IF_GTZ:
  case DOPCODE_IF_LEZ:
    return true;
  default:
    return false;
  }
}

bool is_goto(DexOpcode op) {
  switch (op) {
  case DOPCODE_GOTO_32:
  case DOPCODE_GOTO_16:
  case DOPCODE_GOTO:
    return true;
  default:
    return false;
  }
}

bool is_move(DexOpcode op) {
  return op >= DOPCODE_MOVE && op <= DOPCODE_MOVE_OBJECT_16;
}

DexOpcode invert_conditional_branch(DexOpcode op) {
  switch (op) {
  case DOPCODE_IF_EQ:
    return DOPCODE_IF_NE;
  case DOPCODE_IF_NE:
    return DOPCODE_IF_EQ;
  case DOPCODE_IF_LT:
    return DOPCODE_IF_GE;
  case DOPCODE_IF_GE:
    return DOPCODE_IF_LT;
  case DOPCODE_IF_GT:
    return DOPCODE_IF_LE;
  case DOPCODE_IF_LE:
    return DOPCODE_IF_GT;
  case DOPCODE_IF_EQZ:
    return DOPCODE_IF_NEZ;
  case DOPCODE_IF_NEZ:
    return DOPCODE_IF_EQZ;
  case DOPCODE_IF_LTZ:
    return DOPCODE_IF_GEZ;
  case DOPCODE_IF_GEZ:
    return DOPCODE_IF_LTZ;
  case DOPCODE_IF_GTZ:
    return DOPCODE_IF_LEZ;
  case DOPCODE_IF_LEZ:
    return DOPCODE_IF_GTZ;
  default:
    always_assert_log(false, "Invalid conditional opcode %s", SHOW(op));
  }
}

bit_width_t src_bit_width(DexOpcode op, uint16_t i) {
  switch (dex_opcode::format(op)) {
  case FMT_f00x:
    redex_assert(false);
  case FMT_f10x:
    redex_assert(false);
  case FMT_f12x:
    redex_assert(i == 0);
    return 4;
  case FMT_f12x_2:
    redex_assert(i <= 1);
    return 4;
  case FMT_f11n:
    redex_assert(false);
  case FMT_f11x_d:
    redex_assert(false);
  case FMT_f11x_s:
    redex_assert(i == 0);
    return 8;
  case FMT_f10t:
    redex_assert(false);
  case FMT_f20t:
    redex_assert(false);
  case FMT_f20bc:
    redex_assert(false);
  case FMT_f22x:
    redex_assert(i == 0);
    return 16;
  case FMT_f21t:
    redex_assert(i == 0);
    return 8;
  case FMT_f21s:
    redex_assert(false);
  case FMT_f21h:
    redex_assert(false);
  case FMT_f21c_d:
    redex_assert(false);
  case FMT_f21c_s:
    redex_assert(i == 0);
    return 8;
  case FMT_f23x_d:
    redex_assert(i <= 1);
    return 8;
  case FMT_f23x_s:
    redex_assert(i <= 2);
    return 8;
  case FMT_f22b:
    redex_assert(i == 0);
    return 8;
  case FMT_f22t:
    redex_assert(i <= 1);
    return 4;
  case FMT_f22s:
    redex_assert(i == 0);
    return 4;
  case FMT_f22c_d:
    redex_assert(i == 0);
    return 4;
  case FMT_f22c_s:
    redex_assert(i <= 1);
    return 4;
  case FMT_f22cs:
    redex_assert(false);
  case FMT_f30t:
    redex_assert(false);
  case FMT_f32x:
    redex_assert(i == 0);
    return 16;
  case FMT_f31i:
    redex_assert(false);
  case FMT_f31t:
    redex_assert(i == 0);
    return 8;
  case FMT_f31c:
    redex_assert(false);
  case FMT_f35c:
    redex_assert(i <= 4);
    return 4;
  case FMT_f3rc:
    redex_assert(i == 0);
    return 16;
  case FMT_f41c_d:
    redex_assert(false);
  case FMT_f41c_s:
    redex_assert(i == 0);
    return 16;
  case FMT_f52c_d:
    redex_assert(i == 0);
    return 16;
  case FMT_f52c_s:
    redex_assert(i <= 1);
    return 16;
  case FMT_f5rc:
    redex_assert(i == 0);
    return 16;
  case FMT_f57c:
    redex_assert(i <= 6);
    return 4;
  case FMT_f35ms:
  case FMT_f35mi:
  case FMT_f3rms:
  case FMT_f3rmi:
  case FMT_f51l:
  case FMT_fopcode:
  case FMT_iopcode:
    redex_assert(false);
  }
  not_reached();
}

bit_width_t dest_bit_width(DexOpcode op) {
  switch (dex_opcode::format(op)) {
  case FMT_f00x:
    redex_assert(false);
  case FMT_f10x:
    redex_assert(false);
  case FMT_f12x:    return 4;
  case FMT_f12x_2:  return 4;
  case FMT_f11n:    return 4;
  case FMT_f11x_d:  return 8;
  case FMT_f11x_s:
    redex_assert(false);
  case FMT_f10t:
    redex_assert(false);
  case FMT_f20t:
    redex_assert(false);
  case FMT_f20bc:
    redex_assert(false);
  case FMT_f22x:    return 8;
  case FMT_f21t:
    redex_assert(false);
  case FMT_f21s:    return 8;
  case FMT_f21h:    return 8;
  case FMT_f21c_d:  return 8;
  case FMT_f21c_s:
    redex_assert(false);
  case FMT_f23x_d:  return 8;
  case FMT_f23x_s:
    redex_assert(false);
  case FMT_f22b:    return 8;
  case FMT_f22t:
    redex_assert(false);
  case FMT_f22s:    return 4;
  case FMT_f22c_d:  return 4;
  case FMT_f22c_s:
    redex_assert(false);
  case FMT_f22cs:
    redex_assert(false);
  case FMT_f30t:
    redex_assert(false);
  case FMT_f32x:    return 16;
  case FMT_f31i:    return 8;
  case FMT_f31t:
    redex_assert(false);
  case FMT_f31c:    return 8;
  case FMT_f35c:
    redex_assert(false);
  case FMT_f35ms:
  case FMT_f35mi:
  case FMT_f3rc:
  case FMT_f3rms:
  case FMT_f3rmi:
    redex_assert(false);
  case FMT_f51l:    return 8;
  case FMT_f41c_d:  return 16;
  case FMT_f41c_s:
    redex_assert(false);
  case FMT_f52c_d:  return 16;
  case FMT_f52c_s:
    redex_assert(false);
  case FMT_f5rc:
    redex_assert(false);
  case FMT_f57c:
    redex_assert(false);
  case FMT_fopcode:
    redex_assert(false);
  case FMT_iopcode: return 16;
  }
  not_reached();
}

unsigned dests_size(DexOpcode op) {
  switch (dex_opcode::format(op)) {
  case FMT_f00x:
  case FMT_f10x:
  case FMT_f11x_s:
  case FMT_f10t:
  case FMT_f20t:
  case FMT_f21t:
  case FMT_f21c_s:
  case FMT_f23x_s:
  case FMT_f22t:
  case FMT_f22c_s:
  case FMT_f30t:
  case FMT_f31t:
  case FMT_f35c:
  case FMT_f3rc:
  case FMT_f41c_s:
  case FMT_f52c_s:
  case FMT_f5rc:
  case FMT_f57c:
  case FMT_fopcode:
    return 0;
  case FMT_f12x:
  case FMT_f12x_2:
  case FMT_f11n:
  case FMT_f11x_d:
  case FMT_f22x:
  case FMT_f21s:
  case FMT_f21h:
  case FMT_f21c_d:
  case FMT_f23x_d:
  case FMT_f22b:
  case FMT_f22s:
  case FMT_f22c_d:
  case FMT_f32x:
  case FMT_f31i:
  case FMT_f31c:
  case FMT_f51l:
  case FMT_f41c_d:
  case FMT_f52c_d:
  case FMT_iopcode:
    return 1;
  case FMT_f20bc:
  case FMT_f22cs:
  case FMT_f35ms:
  case FMT_f35mi:
  case FMT_f3rms:
  case FMT_f3rmi:
    always_assert_log(false, "Unimplemented opcode `%s'", SHOW(op));
  }
  not_reached();
}

unsigned min_srcs_size(DexOpcode op) {
  switch (dex_opcode::format(op)) {
  case FMT_f00x:
  case FMT_f10x:
  case FMT_f11n:
  case FMT_f11x_d:
  case FMT_f10t:
  case FMT_f20t:
  case FMT_f21s:
  case FMT_f21h:
  case FMT_f21c_d:
  case FMT_f30t:
  case FMT_f31i:
  case FMT_f31c:
  case FMT_f3rc:
  case FMT_f51l:
  case FMT_f5rc:
  case FMT_f41c_d:
  case FMT_fopcode:
  case FMT_iopcode:
    return 0;
  case FMT_f12x:
  case FMT_f11x_s:
  case FMT_f22x:
  case FMT_f21t:
  case FMT_f21c_s:
  case FMT_f22b:
  case FMT_f22s:
  case FMT_f22c_d:
  case FMT_f32x:
  case FMT_f31t:
  case FMT_f41c_s:
  case FMT_f52c_d:
    return 1;
  case FMT_f12x_2:
  case FMT_f23x_d:
  case FMT_f22t:
  case FMT_f22c_s:
  case FMT_f52c_s:
    return 2;
  case FMT_f23x_s:
    return 3;
  case FMT_f35c:
  case FMT_f57c:
    return 0;
  case FMT_f20bc:
  case FMT_f22cs:
  case FMT_f35ms:
  case FMT_f35mi:
  case FMT_f3rms:
  case FMT_f3rmi:
    always_assert_log(false, "Unimplemented opcode `%s'", SHOW(op));
  }
  not_reached();
}

} // namespace dex_opcode
