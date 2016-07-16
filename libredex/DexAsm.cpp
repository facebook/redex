/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "DexAsm.h"

namespace dex_asm {

bool unsupported(DexOpcode opcode) {
  switch (opcode) {
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_STRING_JUMBO:

  case OPCODE_CONST_CLASS:
  case OPCODE_CHECK_CAST:
  case OPCODE_INSTANCE_OF:
  case OPCODE_NEW_INSTANCE:
  case OPCODE_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY_RANGE:

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
      return true;
    default:
      return false;
  }
}

DexInstruction* dasm(DexOpcode opcode, std::initializer_list<Operand> args) {
  assert(!unsupported(opcode));
  auto insn = new DexInstruction(opcode);
  auto arg = args.begin();
  if (insn->dests_size() && !insn->dest_is_src()) {
    always_assert(arg->tag == VREG);
    insn->set_dest(arg->v);
    arg = std::next(arg);
  }
  for (size_t i = 0; i < insn->srcs_size(); ++i) {
    always_assert(arg->tag == VREG);
    insn->set_src(i, arg->v);
    arg = std::next(arg);
  }
  if (arg != args.end()) {
    switch (arg->tag) {
    case LITERAL:
      insn->set_literal(arg->v);
      break;
    case OFFSET:
      insn->set_offset(arg->v);
      break;
    case VREG:
    default:
      always_assert_log(false, "Encountered unexpected tag 0x%x", arg->tag);
      break;
    }
    arg = std::next(arg);
  }
  always_assert_log(
      arg == args.end(), "Found excess arguments for opcode 0x%x", opcode);
  return insn;
}

}
