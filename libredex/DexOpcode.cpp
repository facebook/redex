/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexOpcode.h"

#include "Debug.h"

namespace opcode {

DexOpcodeFormat format(DexOpcode opcode) {
  switch (opcode) {
#define OP(op, code, fmt, ...) \
  case code:              \
    return FMT_##fmt;
    OPS
#undef OP
  case FOPCODE_PACKED_SWITCH : return FMT_fopcode;
  case FOPCODE_SPARSE_SWITCH:
    return FMT_fopcode;
  case FOPCODE_FILLED_ARRAY:
    return FMT_fopcode;
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE:
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
    return FMT_iopcode;
  }
  always_assert_log(false, "Unexpected opcode 0x%x", opcode);
}

Ref ref(DexOpcode opcode) {
  switch (opcode) {
#define OP(op, code, fmt, ref, ...)\
  case code:              \
    return ref;
    OPS
#undef OP
  case FOPCODE_PACKED_SWITCH:
  case FOPCODE_SPARSE_SWITCH:
  case FOPCODE_FILLED_ARRAY:
  case IOPCODE_LOAD_PARAM:
  case IOPCODE_LOAD_PARAM_OBJECT:
  case IOPCODE_LOAD_PARAM_WIDE:
    // TODO: The load-param opcodes should really contain a type ref. However,
    // for that to happen, a bunch of our analyses that check if certain types
    // are referenced need to be updated.
  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
    return Ref::None;
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
  return op == OPCODE_ADD_INT || op == OPCODE_MUL_INT ||
         (op >= OPCODE_AND_INT && op <= OPCODE_XOR_INT) ||
         op == OPCODE_ADD_LONG || op == OPCODE_MUL_LONG ||
         (op >= OPCODE_AND_LONG && op <= OPCODE_XOR_LONG) ||
         op == OPCODE_ADD_FLOAT || op == OPCODE_MUL_FLOAT ||
         op == OPCODE_ADD_DOUBLE || op == OPCODE_MUL_DOUBLE;
}

bool may_throw(DexOpcode op) {
  switch (op) {
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_STRING_JUMBO:
  case OPCODE_CONST_CLASS:
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_CHECK_CAST:
  case OPCODE_INSTANCE_OF:
  case OPCODE_ARRAY_LENGTH:
  case OPCODE_NEW_INSTANCE:
  case OPCODE_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY_RANGE:
  case OPCODE_AGET:
  case OPCODE_AGET_WIDE:
  case OPCODE_AGET_OBJECT:
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT:
  case OPCODE_APUT:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_OBJECT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT:
  case OPCODE_IGET:
  case OPCODE_IGET_WIDE:
  case OPCODE_IGET_OBJECT:
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_SHORT:
  case OPCODE_IPUT:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT:
  case OPCODE_SGET:
  case OPCODE_SGET_WIDE:
  case OPCODE_SGET_OBJECT:
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT:
  case OPCODE_SPUT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT:
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE:
  case OPCODE_INVOKE_VIRTUAL_RANGE:
  case OPCODE_INVOKE_SUPER_RANGE:
  case OPCODE_INVOKE_DIRECT_RANGE:
  case OPCODE_INVOKE_STATIC_RANGE:
  case OPCODE_INVOKE_INTERFACE_RANGE:
  case OPCODE_DIV_INT:
  case OPCODE_REM_INT:
  case OPCODE_DIV_LONG:
  case OPCODE_REM_LONG:
  case OPCODE_DIV_INT_2ADDR:
  case OPCODE_REM_INT_2ADDR:
  case OPCODE_DIV_LONG_2ADDR:
  case OPCODE_REM_LONG_2ADDR:
  case OPCODE_DIV_INT_LIT16:
  case OPCODE_REM_INT_LIT16:
  case OPCODE_DIV_INT_LIT8:
  case OPCODE_REM_INT_LIT8:
    return true;
  default:
    return false;
  }
}

Branchingness branchingness(DexOpcode op) {
  if (may_throw(op)) {
    return BRANCH_THROW;
  }

  switch (op) {
  case OPCODE_RETURN_VOID:
  case OPCODE_RETURN:
  case OPCODE_RETURN_WIDE:
  case OPCODE_RETURN_OBJECT:
    return BRANCH_RETURN;
  case OPCODE_THROW:
    return BRANCH_THROW;
  case OPCODE_GOTO:
  case OPCODE_GOTO_16:
  case OPCODE_GOTO_32:
    return BRANCH_GOTO;
  case OPCODE_PACKED_SWITCH:
  case OPCODE_SPARSE_SWITCH:
    return BRANCH_SWITCH;
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
    return BRANCH_IF;
  default:
    return BRANCH_NONE;
  }
}

bool has_range_form(DexOpcode op) {
  switch (op) {
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_INTERFACE:
  case OPCODE_FILLED_NEW_ARRAY:
    return true;
  default:
    return false;
  }
}

bool is_internal(DexOpcode op) {
  return format(op) == FMT_iopcode;
}

bool is_load_param(DexOpcode op) {
  return op >= IOPCODE_LOAD_PARAM && op <= IOPCODE_LOAD_PARAM_WIDE;
}

bool is_move_result_pseudo(DexOpcode op) {
  return op >= IOPCODE_MOVE_RESULT_PSEUDO &&
         op <= IOPCODE_MOVE_RESULT_PSEUDO_WIDE;
}

DexOpcode move_result_pseudo_for_iget(DexOpcode op) {
  switch (op) {
    case OPCODE_IGET_BOOLEAN:
    case OPCODE_IGET_BYTE:
    case OPCODE_IGET_SHORT:
    case OPCODE_IGET_CHAR:
    case OPCODE_IGET:
      return IOPCODE_MOVE_RESULT_PSEUDO;
    case OPCODE_IGET_OBJECT:
      return IOPCODE_MOVE_RESULT_PSEUDO_OBJECT;
    case OPCODE_IGET_WIDE:
      return IOPCODE_MOVE_RESULT_PSEUDO_WIDE;
    default:
      always_assert_log(false, "Unexpected opcode %s", SHOW(op));
  }
}

DexOpcode move_result_pseudo_for_sget(DexOpcode op) {
  switch (op) {
    case OPCODE_SGET_BOOLEAN:
    case OPCODE_SGET_BYTE:
    case OPCODE_SGET_SHORT:
    case OPCODE_SGET_CHAR:
    case OPCODE_SGET:
      return IOPCODE_MOVE_RESULT_PSEUDO;
    case OPCODE_SGET_OBJECT:
      return IOPCODE_MOVE_RESULT_PSEUDO_OBJECT;
    case OPCODE_SGET_WIDE:
      return IOPCODE_MOVE_RESULT_PSEUDO_WIDE;
    default:
      always_assert_log(false, "Unexpected opcode %s", SHOW(op));
  }
}

} // namespace opcode

namespace opcode_impl {

unsigned dests_size(DexOpcode op) {
  switch (opcode::format(op)) {
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
  switch (opcode::format(op)) {
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

bit_width_t src_bit_width(DexOpcode op, uint16_t i) {
  switch (opcode::format(op)) {
  case FMT_f00x:    assert(false);
  case FMT_f10x:    assert(false);
  case FMT_f12x:    assert(i == 0); return 4;
  case FMT_f12x_2:  assert(i <= 1); return 4;
  case FMT_f11n:    assert(false);
  case FMT_f11x_d:  assert(false);
  case FMT_f11x_s:  assert(i == 0); return 8;
  case FMT_f10t:    assert(false);
  case FMT_f20t:    assert(false);
  case FMT_f20bc:   assert(false);
  case FMT_f22x:    assert(i == 0); return 16;
  case FMT_f21t:    assert(i == 0); return 8;
  case FMT_f21s:    assert(false);
  case FMT_f21h:    assert(false);
  case FMT_f21c_d:  assert(false);
  case FMT_f21c_s:  assert(i == 0); return 8;
  case FMT_f23x_d:  assert(i <= 1); return 8;
  case FMT_f23x_s:  assert(i <= 2); return 8;
  case FMT_f22b:    assert(i == 0); return 8;
  case FMT_f22t:    assert(i <= 1); return 4;
  case FMT_f22s:    assert(i == 0); return 4;
  case FMT_f22c_d:  assert(i == 0); return 4;
  case FMT_f22c_s:  assert(i <= 1); return 4;
  case FMT_f22cs:   assert(false);
  case FMT_f30t:    assert(false);
  case FMT_f32x:    assert(i == 0); return 16;
  case FMT_f31i:    assert(false);
  case FMT_f31t:    assert(i == 0); return 8;
  case FMT_f31c:    assert(false);
  case FMT_f35c:    assert(i <= 4); return 4;
  case FMT_f3rc:    assert(i == 0); return 16;
  case FMT_f41c_d:  assert(false);
  case FMT_f41c_s:  assert(i == 0);  return 16;
  case FMT_f52c_d:  assert(i == 0);  return 16;
  case FMT_f52c_s:  assert(i <= 1);  return 16;
  case FMT_f5rc:    assert(i == 0);  return 16;
  case FMT_f57c:    assert(i <= 6);  return 4;
  case FMT_f35ms:
  case FMT_f35mi:
  case FMT_f3rms:
  case FMT_f3rmi:
  case FMT_f51l:
  case FMT_fopcode:
  case FMT_iopcode: assert(false);
  }
  not_reached();
}

bit_width_t dest_bit_width(DexOpcode op) {
  switch (opcode::format(op)) {
  case FMT_f00x:    assert(false);
  case FMT_f10x:    assert(false);
  case FMT_f12x:    return 4;
  case FMT_f12x_2:  return 4;
  case FMT_f11n:    return 4;
  case FMT_f11x_d:  return 8;
  case FMT_f11x_s:  assert(false);
  case FMT_f10t:    assert(false);
  case FMT_f20t:    assert(false);
  case FMT_f20bc:   assert(false);
  case FMT_f22x:    return 8;
  case FMT_f21t:    assert(false);
  case FMT_f21s:    return 8;
  case FMT_f21h:    return 8;
  case FMT_f21c_d:  return 8;
  case FMT_f21c_s:  assert(false);
  case FMT_f23x_d:  return 8;
  case FMT_f23x_s:  assert(false);
  case FMT_f22b:    return 8;
  case FMT_f22t:    assert(false);
  case FMT_f22s:    return 4;
  case FMT_f22c_d:  return 4;
  case FMT_f22c_s:  assert(false);
  case FMT_f22cs:   assert(false);
  case FMT_f30t:    assert(false);
  case FMT_f32x:    return 16;
  case FMT_f31i:    return 8;
  case FMT_f31t:    assert(false);
  case FMT_f31c:    return 8;
  case FMT_f35c:    assert(false);
  case FMT_f35ms:
  case FMT_f35mi:
  case FMT_f3rc:
  case FMT_f3rms:
  case FMT_f3rmi:   assert(false);
  case FMT_f51l:    return 8;
  case FMT_f41c_d:  return 16;
  case FMT_f41c_s:  assert(false);
  case FMT_f52c_d:  return 16;
  case FMT_f52c_s:  assert(false);
  case FMT_f5rc:    assert(false);
  case FMT_f57c:    assert(false);
  case FMT_fopcode: assert(false);
  case FMT_iopcode: return 16;
  }
  not_reached();
}

bool dest_is_wide(DexOpcode op) {
  switch (op) {
  case OPCODE_MOVE_WIDE:
  case OPCODE_MOVE_WIDE_FROM16:
  case OPCODE_MOVE_WIDE_16:
  case OPCODE_MOVE_RESULT_WIDE:

  case OPCODE_CONST_WIDE_16:
  case OPCODE_CONST_WIDE_32:
  case OPCODE_CONST_WIDE:
  case OPCODE_CONST_WIDE_HIGH16:

  case OPCODE_AGET_WIDE:
  case OPCODE_IGET_WIDE:
  case OPCODE_SGET_WIDE:

  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG:
  case OPCODE_NEG_DOUBLE:
  case OPCODE_INT_TO_LONG:
  case OPCODE_INT_TO_DOUBLE:
  case OPCODE_LONG_TO_DOUBLE:
  case OPCODE_FLOAT_TO_LONG:
  case OPCODE_FLOAT_TO_DOUBLE:
  case OPCODE_DOUBLE_TO_LONG:

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

  case IOPCODE_MOVE_RESULT_PSEUDO:
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    return false;
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
    return true;

  default:
    return false;
  }
}

bool dest_is_object(DexOpcode op) {
  switch (op) {
  case OPCODE_NOP:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_MOVE:
  case OPCODE_MOVE_WIDE:
    return false;
  case OPCODE_MOVE_OBJECT:
    return true;
  case OPCODE_MOVE_RESULT:
  case OPCODE_MOVE_RESULT_WIDE:
    return false;
  case OPCODE_MOVE_RESULT_OBJECT:
  case OPCODE_MOVE_EXCEPTION:
    return true;
  case OPCODE_RETURN_VOID:
  case OPCODE_RETURN:
  case OPCODE_RETURN_WIDE:
  case OPCODE_RETURN_OBJECT:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_CONST_4:
    return false;
  case OPCODE_MONITOR_ENTER:
  case OPCODE_MONITOR_EXIT:
  case OPCODE_THROW:
  case OPCODE_GOTO:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_NEG_INT:
  case OPCODE_NOT_INT:
  case OPCODE_NEG_LONG:
  case OPCODE_NOT_LONG:
  case OPCODE_NEG_FLOAT:
  case OPCODE_NEG_DOUBLE:
  case OPCODE_INT_TO_LONG:
  case OPCODE_INT_TO_FLOAT:
  case OPCODE_INT_TO_DOUBLE:
  case OPCODE_LONG_TO_INT:
  case OPCODE_LONG_TO_FLOAT:
  case OPCODE_LONG_TO_DOUBLE:
  case OPCODE_FLOAT_TO_INT:
  case OPCODE_FLOAT_TO_LONG:
  case OPCODE_FLOAT_TO_DOUBLE:
  case OPCODE_DOUBLE_TO_INT:
  case OPCODE_DOUBLE_TO_LONG:
  case OPCODE_DOUBLE_TO_FLOAT:
  case OPCODE_INT_TO_BYTE:
  case OPCODE_INT_TO_CHAR:
  case OPCODE_INT_TO_SHORT:
  case OPCODE_ADD_INT_2ADDR:
  case OPCODE_SUB_INT_2ADDR:
  case OPCODE_MUL_INT_2ADDR:
  case OPCODE_DIV_INT_2ADDR:
  case OPCODE_REM_INT_2ADDR:
  case OPCODE_AND_INT_2ADDR:
  case OPCODE_OR_INT_2ADDR:
  case OPCODE_XOR_INT_2ADDR:
  case OPCODE_SHL_INT_2ADDR:
  case OPCODE_SHR_INT_2ADDR:
  case OPCODE_USHR_INT_2ADDR:
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
  case OPCODE_ADD_FLOAT_2ADDR:
  case OPCODE_SUB_FLOAT_2ADDR:
  case OPCODE_MUL_FLOAT_2ADDR:
  case OPCODE_DIV_FLOAT_2ADDR:
  case OPCODE_REM_FLOAT_2ADDR:
  case OPCODE_ADD_DOUBLE_2ADDR:
  case OPCODE_SUB_DOUBLE_2ADDR:
  case OPCODE_MUL_DOUBLE_2ADDR:
  case OPCODE_DIV_DOUBLE_2ADDR:
  case OPCODE_REM_DOUBLE_2ADDR:
  case OPCODE_ARRAY_LENGTH:
  case OPCODE_MOVE_FROM16:
  case OPCODE_MOVE_WIDE_FROM16:
    return false;
  case OPCODE_MOVE_OBJECT_FROM16:
    return true;
  case OPCODE_CONST_16:
  case OPCODE_CONST_HIGH16:
  case OPCODE_CONST_WIDE_16:
  case OPCODE_CONST_WIDE_HIGH16:
    return false;
  case OPCODE_GOTO_16:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_CMPL_FLOAT:
  case OPCODE_CMPG_FLOAT:
  case OPCODE_CMPL_DOUBLE:
  case OPCODE_CMPG_DOUBLE:
  case OPCODE_CMP_LONG:
    return false;
  case OPCODE_IF_EQ:
  case OPCODE_IF_NE:
  case OPCODE_IF_LT:
  case OPCODE_IF_GE:
  case OPCODE_IF_GT:
  case OPCODE_IF_LE:
  case OPCODE_IF_EQZ:
  case OPCODE_IF_NEZ:
  case OPCODE_IF_LTZ:
  case OPCODE_IF_GEZ:
  case OPCODE_IF_GTZ:
  case OPCODE_IF_LEZ:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_AGET:
  case OPCODE_AGET_WIDE:
    return false;
  case OPCODE_AGET_OBJECT:
    return true;
  case OPCODE_AGET_BOOLEAN:
  case OPCODE_AGET_BYTE:
  case OPCODE_AGET_CHAR:
  case OPCODE_AGET_SHORT:
    return false;
  case OPCODE_APUT:
  case OPCODE_APUT_WIDE:
  case OPCODE_APUT_OBJECT:
  case OPCODE_APUT_BOOLEAN:
  case OPCODE_APUT_BYTE:
  case OPCODE_APUT_CHAR:
  case OPCODE_APUT_SHORT:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_ADD_INT:
  case OPCODE_SUB_INT:
  case OPCODE_MUL_INT:
  case OPCODE_DIV_INT:
  case OPCODE_REM_INT:
  case OPCODE_AND_INT:
  case OPCODE_OR_INT:
  case OPCODE_XOR_INT:
  case OPCODE_SHL_INT:
  case OPCODE_SHR_INT:
  case OPCODE_USHR_INT:
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
  case OPCODE_ADD_FLOAT:
  case OPCODE_SUB_FLOAT:
  case OPCODE_MUL_FLOAT:
  case OPCODE_DIV_FLOAT:
  case OPCODE_REM_FLOAT:
  case OPCODE_ADD_DOUBLE:
  case OPCODE_SUB_DOUBLE:
  case OPCODE_MUL_DOUBLE:
  case OPCODE_DIV_DOUBLE:
  case OPCODE_REM_DOUBLE:
    return false;
  case OPCODE_ADD_INT_LIT16:
  case OPCODE_RSUB_INT:
  case OPCODE_MUL_INT_LIT16:
  case OPCODE_DIV_INT_LIT16:
  case OPCODE_REM_INT_LIT16:
  case OPCODE_AND_INT_LIT16:
  case OPCODE_OR_INT_LIT16:
  case OPCODE_XOR_INT_LIT16:
  case OPCODE_ADD_INT_LIT8:
  case OPCODE_RSUB_INT_LIT8:
  case OPCODE_MUL_INT_LIT8:
  case OPCODE_DIV_INT_LIT8:
  case OPCODE_REM_INT_LIT8:
  case OPCODE_AND_INT_LIT8:
  case OPCODE_OR_INT_LIT8:
  case OPCODE_XOR_INT_LIT8:
  case OPCODE_SHL_INT_LIT8:
  case OPCODE_SHR_INT_LIT8:
  case OPCODE_USHR_INT_LIT8:
  case OPCODE_MOVE_16:
  case OPCODE_MOVE_WIDE_16:
    return false;
  case OPCODE_MOVE_OBJECT_16:
    return true;
  case OPCODE_CONST:
  case OPCODE_CONST_WIDE_32:
    return false;
  case OPCODE_FILL_ARRAY_DATA:
  case OPCODE_GOTO_32:
  case OPCODE_PACKED_SWITCH:
  case OPCODE_SPARSE_SWITCH:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_CONST_WIDE:
  case OPCODE_IGET:
  case OPCODE_IGET_WIDE:
    return false;
  case OPCODE_IGET_OBJECT:
    return true;
  case OPCODE_IGET_BOOLEAN:
  case OPCODE_IGET_BYTE:
  case OPCODE_IGET_CHAR:
  case OPCODE_IGET_SHORT:
    return false;
  case OPCODE_IPUT:
  case OPCODE_IPUT_WIDE:
  case OPCODE_IPUT_OBJECT:
  case OPCODE_IPUT_BOOLEAN:
  case OPCODE_IPUT_BYTE:
  case OPCODE_IPUT_CHAR:
  case OPCODE_IPUT_SHORT:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_SGET:
  case OPCODE_SGET_WIDE:
    return false;
  case OPCODE_SGET_OBJECT:
    return true;
  case OPCODE_SGET_BOOLEAN:
  case OPCODE_SGET_BYTE:
  case OPCODE_SGET_CHAR:
  case OPCODE_SGET_SHORT:
    return false;
  case OPCODE_SPUT:
  case OPCODE_SPUT_WIDE:
  case OPCODE_SPUT_OBJECT:
  case OPCODE_SPUT_BOOLEAN:
  case OPCODE_SPUT_BYTE:
  case OPCODE_SPUT_CHAR:
  case OPCODE_SPUT_SHORT:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_INVOKE_VIRTUAL:
  case OPCODE_INVOKE_SUPER:
  case OPCODE_INVOKE_DIRECT:
  case OPCODE_INVOKE_STATIC:
  case OPCODE_INVOKE_INTERFACE:
  case OPCODE_INVOKE_VIRTUAL_RANGE:
  case OPCODE_INVOKE_SUPER_RANGE:
  case OPCODE_INVOKE_DIRECT_RANGE:
  case OPCODE_INVOKE_STATIC_RANGE:
  case OPCODE_INVOKE_INTERFACE_RANGE:
    always_assert_log(false, "No dest");
    not_reached();
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_STRING_JUMBO:
  case OPCODE_CONST_CLASS:
  case OPCODE_CHECK_CAST:
    return true;
  case OPCODE_INSTANCE_OF:
    return false;
  case OPCODE_NEW_INSTANCE:
  case OPCODE_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY_RANGE:
    return true;
  case IOPCODE_LOAD_PARAM:
    return false;
  case IOPCODE_LOAD_PARAM_OBJECT:
    return true;
  case IOPCODE_LOAD_PARAM_WIDE:
    return false;
  case IOPCODE_MOVE_RESULT_PSEUDO:
    return false;
  case IOPCODE_MOVE_RESULT_PSEUDO_OBJECT:
    return true;
  case IOPCODE_MOVE_RESULT_PSEUDO_WIDE:
    return false;
  default:
    always_assert_log(false, "Unknown opcode %02x\n", op);
  }
}

} // namespace opcode_impl
