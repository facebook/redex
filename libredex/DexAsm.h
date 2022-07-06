/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <initializer_list>

#include "IRInstruction.h"

/*
 * Mini-DSL for building DexInstructions quickly.
 */

namespace dex_asm {

enum OperandTag { VREG, LITERAL };

struct Operand {
  OperandTag tag;
  int64_t v;
};

inline Operand operator"" _v(unsigned long long v) {
  return {VREG, static_cast<int64_t>(v)};
}

inline Operand operator"" _L(unsigned long long v) {
  return {LITERAL, static_cast<int64_t>(v)};
}

bool unsupported(IROpcode opcode);

template <typename Iterator>
void assemble(IRInstruction* insn, Iterator begin, Iterator end) {
  auto arg = begin;
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
  if (arg != end) {
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
  always_assert_log(arg == end, "Found excess arguments for opcode 0x%x",
                    insn->opcode());
}

IRInstruction* dasm(IROpcode opcode, std::initializer_list<Operand> = {});
IRInstruction* dasm(IROpcode opcode,
                    const DexString* string,
                    std::initializer_list<Operand> = {});
IRInstruction* dasm(IROpcode opcode,
                    DexType* type,
                    std::initializer_list<Operand> = {});
IRInstruction* dasm(IROpcode opcode,
                    DexFieldRef* field,
                    std::initializer_list<Operand> = {});
IRInstruction* dasm(IROpcode opcode,
                    DexMethodRef* method,
                    std::initializer_list<Operand> = {});

template <typename Iterator>
IRInstruction* dasm(IROpcode opcode,
                    DexMethodRef* method,
                    Iterator begin,
                    Iterator end) {
  auto insn = new IRInstruction(opcode);
  insn->set_method(method);
  insn->set_srcs_size(std::distance(begin, end));
  assemble(insn, begin, end);
  return insn;
}
} // namespace dex_asm
