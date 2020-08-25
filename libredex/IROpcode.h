/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstdint>
#include <string>

#include "Show.h"

class DexField;

enum DexOpcode : uint16_t;

namespace opcode {

enum class Ref {
  None,
  Literal,
  String,
  Type,
  Field,
  Method,
  CallSite,
  MethodHandle,
  Data,
};

} // namespace opcode

enum IROpcode : uint16_t {
#define OP(op, code, ...) OPCODE_##op,
#include "IROpcodes.def"
  IOPCODE_LOAD_PARAM,
  IOPCODE_LOAD_PARAM_OBJECT,
  IOPCODE_LOAD_PARAM_WIDE,

  IOPCODE_MOVE_RESULT_PSEUDO,
  IOPCODE_MOVE_RESULT_PSEUDO_OBJECT,
  IOPCODE_MOVE_RESULT_PSEUDO_WIDE,
};
// clang-format on

std::string show(IROpcode);

using bit_width_t = uint8_t;

namespace opcode {

Ref ref(IROpcode);

/*
 * 2addr and non-2addr DexOpcode pairs will get mapped to the same IROpcode.
 * range and non-range DexOpcode pairs will get mapped to the same IROpcode.
 * goto, goto/16, and goto/32 will get mapped to the same IROpcode.
 * move, move/from16, and move/16 will get mapped to the same IROpcode. Same
 * for the move-object and move-wide opcode families.
 * const/4, const/16, and const will all get mapped to the same IROpcode. Same
 * for the const-wide opcode family.
 * All other DexOpcodes have a 1-1 mapping with an IROpcode.
 */
IROpcode from_dex_opcode(DexOpcode);

/*
 * Only non-internal IROpcodes are valid inputs to this function.
 *
 * This function is roughly the inverse of `from_dex_opcode`. When there are
 * are multiple DexOpcodes that map to a single IROpcode, we pick one of them
 * to return here.
 */
DexOpcode to_dex_opcode(IROpcode);

bool may_throw(IROpcode);

inline bool can_throw(IROpcode op) {
  return may_throw(op) || op == OPCODE_THROW;
}

// if an IROpcode can be translated to a DexOpcode of /range format
bool has_range_form(IROpcode);

DexOpcode range_version(IROpcode);

bool has_variable_srcs_size(IROpcode op);

inline bool is_const_string(IROpcode op) { return op == OPCODE_CONST_STRING; }

// Internal opcodes cannot be mapped to a corresponding DexOpcode.
bool is_internal(IROpcode);

bool is_load_param(IROpcode);

inline bool is_move_result(IROpcode op) {
  return op >= OPCODE_MOVE_RESULT && op <= OPCODE_MOVE_RESULT_OBJECT;
}

bool is_move_result_pseudo(IROpcode);

bool is_move_result_any(IROpcode op);

bool is_move(IROpcode);

bool is_commutative(IROpcode opcode);

bool is_cmp(IROpcode opcode);

bool is_binop64(IROpcode op);

IROpcode load_param_to_move(IROpcode);

IROpcode iget_to_move(IROpcode);

IROpcode iput_to_move(IROpcode);

IROpcode invert_conditional_branch(IROpcode op);

IROpcode move_result_pseudo_for_iget(IROpcode op);

IROpcode move_result_pseudo_for_sget(IROpcode op);

IROpcode move_result_for_invoke(const DexMethodRef* method);

IROpcode invoke_for_method(const DexMethod* method);

IROpcode return_opcode(const DexType* type);

IROpcode load_opcode(const DexType* type);

IROpcode move_result_to_move(IROpcode);

IROpcode return_to_move(IROpcode);

IROpcode move_result_to_pseudo(IROpcode op);

IROpcode pseudo_to_move_result(IROpcode op);

IROpcode sget_opcode_for_field(const DexField* field);

enum Branchingness {
  BRANCH_NONE,
  BRANCH_RETURN,
  BRANCH_GOTO,
  BRANCH_IF,
  BRANCH_SWITCH,
  BRANCH_THROW // both always throw and may_throw
};

Branchingness branchingness(IROpcode op);

/*
 * These instructions have observable side effects so must always be considered
 * live, regardless of whether their output is consumed by another instruction.
 */
bool has_side_effects(IROpcode opc);

} // namespace opcode

/*
 * The functions below probably shouldn't be accessed directly by opt/ code.
 * They're implementation details wrapped by IRInstruction and DexInstruction.
 */
namespace opcode_impl {

bool has_dest(IROpcode);

bool has_move_result_pseudo(IROpcode);

// we can't tell the srcs size from the opcode alone -- format 35c opcodes
// encode that separately. So this just returns the minimum.
unsigned min_srcs_size(IROpcode);

bool dest_is_wide(IROpcode);

bool dest_is_object(IROpcode);

} // namespace opcode_impl
