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
#define OP(op, code, fmt) \
  case code:              \
    return FMT_##fmt;
    OPS
#undef OP
  case FOPCODE_PACKED_SWITCH : return FMT_fopcode;
  case FOPCODE_SPARSE_SWITCH:
    return FMT_fopcode;
  case FOPCODE_FILLED_ARRAY:
    return FMT_fopcode;
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

} // namespace opcode
