/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexOpcodeDefs.h"

#include <sstream>
#include <stdexcept>

std::string print(DexOpcode opcode) {
   switch (opcode) {
 #define OP(op, code, fmt, literal) \
   case DOPCODE_##op:               \
     return #literal;
     DOPS
     QDOPS
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
    throw std::invalid_argument("Can't quicken opcode.");
  }
}
