/*
 * Copyright (c) Facebook, Inc. and its affiliates.
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
} // namespace dex_asm
