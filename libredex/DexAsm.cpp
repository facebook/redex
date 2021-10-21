/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "DexAsm.h"

#include "IRInstruction.h"
#include "Show.h"

namespace dex_asm {

bool unsupported(IROpcode opcode) {
  switch (opcode) {
  case OPCODE_CONST_STRING:
  case OPCODE_CONST_CLASS:
  case OPCODE_CHECK_CAST:
  case OPCODE_INSTANCE_OF:
  case OPCODE_NEW_INSTANCE:
  case OPCODE_NEW_ARRAY:
  case OPCODE_FILLED_NEW_ARRAY:

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
    return true;
  default:
    return false;
  }
}

void assemble(IRInstruction* insn, std::initializer_list<Operand> args) {
  auto arg = args.begin();
  if (insn->has_dest()) {
    always_assert(arg->tag == VREG);
    insn->set_dest(arg->v);
    ++arg;
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
    case VREG:
    default:
      not_reached_log("Encountered unexpected tag 0x%x", arg->tag);
    }
    arg = std::next(arg);
  }
  always_assert_log(arg == args.end(),
                    "Found excess arguments for opcode 0x%x",
                    insn->opcode());
}

IRInstruction* dasm(IROpcode opcode, std::initializer_list<Operand> args) {
  assert_log(!unsupported(opcode), "%s is unsupported", SHOW(opcode));
  auto insn = new IRInstruction(opcode);
  assemble(insn, args);
  return insn;
}

IRInstruction* dasm(IROpcode opcode,
                    const DexString* string,
                    std::initializer_list<Operand> args) {
  auto insn = new IRInstruction(opcode);
  insn->set_string(string);
  assemble(insn, args);
  return insn;
}

IRInstruction* dasm(IROpcode opcode,
                    DexType* type,
                    std::initializer_list<Operand> args) {
  auto insn = new IRInstruction(opcode);
  insn->set_type(type);
  assemble(insn, args);
  return insn;
}

IRInstruction* dasm(IROpcode opcode,
                    DexFieldRef* field,
                    std::initializer_list<Operand> args) {
  auto insn = new IRInstruction(opcode);
  insn->set_field(field);
  assemble(insn, args);
  return insn;
}

IRInstruction* dasm(IROpcode opcode,
                    DexMethodRef* method,
                    std::initializer_list<Operand> args) {
  auto insn = new IRInstruction(opcode);
  insn->set_method(method);
  insn->set_srcs_size(args.size());
  assemble(insn, args);
  return insn;
}
} // namespace dex_asm
