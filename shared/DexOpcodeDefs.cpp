/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexOpcodeDefs.h"

#include <sstream>
#include <stdexcept>

std::string print(DexOpcode opcode) {
  /* clang-format off */

  switch (opcode) {
#define OP(op, code, fmt, literal) \
  case DOPCODE_##op:               \
    return #literal;
    DOPS QDOPS
#undef OP

  case FOPCODE_PACKED_SWITCH:
    return "PACKED_SWITCH_DATA";
  case FOPCODE_SPARSE_SWITCH:
    return "SPARSE_SWITCH_DATA";
  case FOPCODE_FILLED_ARRAY:
    return "FILLED_ARRAY_DATA";
  default:
    return "NO_VALID_OPCODE";
  }

  /* clang-format on */
}

DexOpcode quicken(DexOpcode opcode) {
  switch (opcode) {
  case DOPCODE_RETURN_VOID:
    return DOPCODE_RETURN_VOID_NO_BARRIER;
  case DOPCODE_IGET:
    return DOPCODE_IGET_QUICK;
  case DOPCODE_IGET_WIDE:
    return DOPCODE_IGET_WIDE_QUICK;
  case DOPCODE_IGET_OBJECT:
    return DOPCODE_IGET_OBJECT_QUICK;
  case DOPCODE_IGET_BOOLEAN:
    return DOPCODE_IGET_BOOLEAN_QUICK;
  case DOPCODE_IGET_BYTE:
    return DOPCODE_IGET_BYTE_QUICK;
  case DOPCODE_IGET_CHAR:
    return DOPCODE_IGET_CHAR_QUICK;
  case DOPCODE_IGET_SHORT:
    return DOPCODE_IGET_SHORT_QUICK;

  case DOPCODE_IPUT:
    return DOPCODE_IPUT_QUICK;
  case DOPCODE_IPUT_WIDE:
    return DOPCODE_IPUT_WIDE_QUICK;
  case DOPCODE_IPUT_OBJECT:
    return DOPCODE_IPUT_OBJECT_QUICK;
  case DOPCODE_IPUT_BOOLEAN:
    return DOPCODE_IPUT_BOOLEAN_QUICK;
  case DOPCODE_IPUT_BYTE:
    return DOPCODE_IPUT_BYTE_QUICK;
  case DOPCODE_IPUT_CHAR:
    return DOPCODE_IPUT_CHAR_QUICK;
  case DOPCODE_IPUT_SHORT:
    return DOPCODE_IPUT_SHORT_QUICK;

  default:
    std::ostringstream msg;
    msg << std::string("Can't quicken opcode: ") << std::hex
        << (uint16_t)opcode;
    throw std::invalid_argument(msg.str());
  }
}
