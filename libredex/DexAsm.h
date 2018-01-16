/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include <cstdint>
#include <initializer_list>

#include "IRInstruction.h"

/*
 * Mini-DSL for building DexInstructions quickly.
 */

namespace dex_asm {

enum OperandTag {
  VREG,
  LITERAL
};

struct Operand {
  OperandTag tag;
  uint64_t v;
};

inline Operand operator "" _v(unsigned long long v) {
  return {VREG, v};
}

inline Operand operator"" _L(unsigned long long v) { return {LITERAL, v}; }

IRInstruction* dasm(IROpcode opcode, std::initializer_list<Operand> = {});
IRInstruction* dasm(IROpcode opcode,
                    DexString* string,
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
} // namespace dex_asm
