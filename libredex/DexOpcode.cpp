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
    return Ref::None;
  }
  always_assert_log(false, "Unexpected opcode 0x%x", opcode);
}

unsigned dests_size(DexOpcode op) {
  switch (format(op)) {
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
  switch (format(op)) {
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

bool is_load_param(DexOpcode op) {
  // currently the only internal opcodes are the load param opcodes
  return format(op) == FMT_iopcode;
}

} // namespace opcode
