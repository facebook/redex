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
