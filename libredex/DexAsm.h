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
  OFFSET,
  LITERAL
};

struct Operand {
  OperandTag tag;
  uint64_t v;
};

inline Operand operator "" _v(unsigned long long v) {
  return {VREG, v};
}

inline Operand operator "" _off(unsigned long long v) {
  return {OFFSET, v};
}

inline Operand operator "" _L(unsigned long long v) {
  return {LITERAL, v};
}

IRInstruction* dasm(DexOpcode opcode, std::initializer_list<Operand> = {});
IRStringInstruction* dasm(DexOpcode opcode,
                          DexString* string,
                          std::initializer_list<Operand> = {});
IRTypeInstruction* dasm(DexOpcode opcode,
                        DexType* type,
                        std::initializer_list<Operand> = {});
IRFieldInstruction* dasm(DexOpcode opcode,
                         DexField* field,
                         std::initializer_list<Operand> = {});
IRMethodInstruction* dasm(DexOpcode opcode,
                          DexMethod* method,
                          std::initializer_list<Operand> = {});
}
