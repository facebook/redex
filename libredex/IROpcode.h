/**
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
  Data,
};

} // namespace opcode

#define OPS \
  OP(NOP               , Ref::None, "nop") \
  OP(MOVE              , Ref::None, "move") \
  OP(MOVE_WIDE         , Ref::None, "move-wide") \
  OP(MOVE_OBJECT       , Ref::None, "move-object") \
  OP(MOVE_RESULT       , Ref::None, "move-result") \
  OP(MOVE_RESULT_WIDE  , Ref::None, "move-result-wide") \
  OP(MOVE_RESULT_OBJECT, Ref::None, "move-result-object") \
  OP(MOVE_EXCEPTION    , Ref::None, "move-exception") \
  OP(RETURN_VOID       , Ref::None, "return-void") \
  OP(RETURN            , Ref::None, "return") \
  OP(RETURN_WIDE       , Ref::None, "return-wide") \
  OP(RETURN_OBJECT     , Ref::None, "return-object") \
  OP(CONST             , Ref::Literal, "const") \
  OP(CONST_WIDE        , Ref::Literal, "const-wide") \
  OP(CONST_STRING      , Ref::String, "const-string") \
  OP(CONST_CLASS       , Ref::Type, "const-class") \
  OP(MONITOR_ENTER     , Ref::None, "monitor-enter") \
  OP(MONITOR_EXIT      , Ref::None, "monitor-exit") \
  OP(CHECK_CAST        , Ref::Type, "check-cast") \
  OP(INSTANCE_OF       , Ref::Type, "instance-of") \
  OP(ARRAY_LENGTH      , Ref::None, "array-length") \
  OP(NEW_INSTANCE      , Ref::Type, "new-instance") \
  OP(NEW_ARRAY         , Ref::Type, "new-array") \
  OP(FILLED_NEW_ARRAY  , Ref::Type, "filled-new-array") \
  OP(FILL_ARRAY_DATA   , Ref::Data, "fill-array-data") \
  OP(THROW             , Ref::None, "throw") \
  OP(GOTO              , Ref::None, "goto") \
  OP(PACKED_SWITCH     , Ref::None, "packed-switch") \
  OP(SPARSE_SWITCH     , Ref::None, "sparse-switch") \
  OP(CMPL_FLOAT        , Ref::None, "cmpl-float") \
  OP(CMPG_FLOAT        , Ref::None, "cmpg-float") \
  OP(CMPL_DOUBLE       , Ref::None, "cmpl-double") \
  OP(CMPG_DOUBLE       , Ref::None, "cmpg-double") \
  OP(CMP_LONG          , Ref::None, "cmp-long") \
  OP(IF_EQ             , Ref::None, "if-eq") \
  OP(IF_NE             , Ref::None, "if-ne") \
  OP(IF_LT             , Ref::None, "if-lt") \
  OP(IF_GE             , Ref::None, "if-ge") \
  OP(IF_GT             , Ref::None, "if-gt") \
  OP(IF_LE             , Ref::None, "if-le") \
  OP(IF_EQZ            , Ref::None, "if-eqz") \
  OP(IF_NEZ            , Ref::None, "if-nez") \
  OP(IF_LTZ            , Ref::None, "if-ltz") \
  OP(IF_GEZ            , Ref::None, "if-gez") \
  OP(IF_GTZ            , Ref::None, "if-gtz") \
  OP(IF_LEZ            , Ref::None, "if-lez") \
  OP(AGET              , Ref::None, "aget") \
  OP(AGET_WIDE         , Ref::None, "aget-wide") \
  OP(AGET_OBJECT       , Ref::None, "aget-object") \
  OP(AGET_BOOLEAN      , Ref::None, "aget-boolean") \
  OP(AGET_BYTE         , Ref::None, "aget-byte") \
  OP(AGET_CHAR         , Ref::None, "aget-char") \
  OP(AGET_SHORT        , Ref::None, "aget-short") \
  OP(APUT              , Ref::None, "aput") \
  OP(APUT_WIDE         , Ref::None, "aput-wide") \
  OP(APUT_OBJECT       , Ref::None, "aput-object") \
  OP(APUT_BOOLEAN      , Ref::None, "aput-boolean") \
  OP(APUT_BYTE         , Ref::None, "aput-byte") \
  OP(APUT_CHAR         , Ref::None, "aput-char") \
  OP(APUT_SHORT        , Ref::None, "aput-short") \
  OP(IGET              , Ref::Field, "iget") \
  OP(IGET_WIDE         , Ref::Field, "iget-wide") \
  OP(IGET_OBJECT       , Ref::Field, "iget-object") \
  OP(IGET_BOOLEAN      , Ref::Field, "iget-boolean") \
  OP(IGET_BYTE         , Ref::Field, "iget-byte") \
  OP(IGET_CHAR         , Ref::Field, "iget-char") \
  OP(IGET_SHORT        , Ref::Field, "iget-short") \
  OP(IPUT              , Ref::Field, "iput") \
  OP(IPUT_WIDE         , Ref::Field, "iput-wide") \
  OP(IPUT_OBJECT       , Ref::Field, "iput-object") \
  OP(IPUT_BOOLEAN      , Ref::Field, "iput-boolean") \
  OP(IPUT_BYTE         , Ref::Field, "iput-byte") \
  OP(IPUT_CHAR         , Ref::Field, "iput-char") \
  OP(IPUT_SHORT        , Ref::Field, "iput-short") \
  OP(SGET              , Ref::Field, "sget") \
  OP(SGET_WIDE         , Ref::Field, "sget-wide") \
  OP(SGET_OBJECT       , Ref::Field, "sget-object") \
  OP(SGET_BOOLEAN      , Ref::Field, "sget-boolean") \
  OP(SGET_BYTE         , Ref::Field, "sget-byte") \
  OP(SGET_CHAR         , Ref::Field, "sget-char") \
  OP(SGET_SHORT        , Ref::Field, "sget-short") \
  OP(SPUT              , Ref::Field, "sput") \
  OP(SPUT_WIDE         , Ref::Field, "sput-wide") \
  OP(SPUT_OBJECT       , Ref::Field, "sput-object") \
  OP(SPUT_BOOLEAN      , Ref::Field, "sput-boolean") \
  OP(SPUT_BYTE         , Ref::Field, "sput-byte") \
  OP(SPUT_CHAR         , Ref::Field, "sput-char") \
  OP(SPUT_SHORT        , Ref::Field, "sput-short") \
  OP(INVOKE_VIRTUAL    , Ref::Method, "invoke-virtual") \
  OP(INVOKE_SUPER      , Ref::Method, "invoke-super") \
  OP(INVOKE_DIRECT     , Ref::Method, "invoke-direct") \
  OP(INVOKE_STATIC     , Ref::Method, "invoke-static") \
  OP(INVOKE_INTERFACE  , Ref::Method, "invoke-interface") \
  OP(NEG_INT           , Ref::None, "neg-int") \
  OP(NOT_INT           , Ref::None, "not-int") \
  OP(NEG_LONG          , Ref::None, "neg-long") \
  OP(NOT_LONG          , Ref::None, "not-long") \
  OP(NEG_FLOAT         , Ref::None, "neg-float") \
  OP(NEG_DOUBLE        , Ref::None, "neg-double") \
  OP(INT_TO_LONG       , Ref::None, "int-to-long") \
  OP(INT_TO_FLOAT      , Ref::None, "int-to-float") \
  OP(INT_TO_DOUBLE     , Ref::None, "int-to-double") \
  OP(LONG_TO_INT       , Ref::None, "long-to-int") \
  OP(LONG_TO_FLOAT     , Ref::None, "long-to-float") \
  OP(LONG_TO_DOUBLE    , Ref::None, "long-to-double") \
  OP(FLOAT_TO_INT      , Ref::None, "float-to-int") \
  OP(FLOAT_TO_LONG     , Ref::None, "float-to-long") \
  OP(FLOAT_TO_DOUBLE   , Ref::None, "float-to-double") \
  OP(DOUBLE_TO_INT     , Ref::None, "double-to-int") \
  OP(DOUBLE_TO_LONG    , Ref::None, "double-to-long") \
  OP(DOUBLE_TO_FLOAT   , Ref::None, "double-to-float") \
  OP(INT_TO_BYTE       , Ref::None, "int-to-byte") \
  OP(INT_TO_CHAR       , Ref::None, "int-to-char") \
  OP(INT_TO_SHORT      , Ref::None, "int-to-short") \
  OP(ADD_INT           , Ref::None, "add-int") \
  OP(SUB_INT           , Ref::None, "sub-int") \
  OP(MUL_INT           , Ref::None, "mul-int") \
  OP(DIV_INT           , Ref::None, "div-int") \
  OP(REM_INT           , Ref::None, "rem-int") \
  OP(AND_INT           , Ref::None, "and-int") \
  OP(OR_INT            , Ref::None, "or-int") \
  OP(XOR_INT           , Ref::None, "xor-int") \
  OP(SHL_INT           , Ref::None, "shl-int") \
  OP(SHR_INT           , Ref::None, "shr-int") \
  OP(USHR_INT          , Ref::None, "ushr-int") \
  OP(ADD_LONG          , Ref::None, "add-long") \
  OP(SUB_LONG          , Ref::None, "sub-long") \
  OP(MUL_LONG          , Ref::None, "mul-long") \
  OP(DIV_LONG          , Ref::None, "div-long") \
  OP(REM_LONG          , Ref::None, "rem-long") \
  OP(AND_LONG          , Ref::None, "and-long") \
  OP(OR_LONG           , Ref::None, "or-long") \
  OP(XOR_LONG          , Ref::None, "xor-long") \
  OP(SHL_LONG          , Ref::None, "shl-long") \
  OP(SHR_LONG          , Ref::None, "shr-long") \
  OP(USHR_LONG         , Ref::None, "ushr-long") \
  OP(ADD_FLOAT         , Ref::None, "add-float") \
  OP(SUB_FLOAT         , Ref::None, "sub-float") \
  OP(MUL_FLOAT         , Ref::None, "mul-float") \
  OP(DIV_FLOAT         , Ref::None, "div-float") \
  OP(REM_FLOAT         , Ref::None, "rem-float") \
  OP(ADD_DOUBLE        , Ref::None, "add-double") \
  OP(SUB_DOUBLE        , Ref::None, "sub-double") \
  OP(MUL_DOUBLE        , Ref::None, "mul-double") \
  OP(DIV_DOUBLE        , Ref::None, "div-double") \
  OP(REM_DOUBLE        , Ref::None, "rem-double") \
  OP(ADD_INT_LIT16     , Ref::Literal, "add-int/lit16") \
  OP(RSUB_INT          , Ref::Literal, "rsub-int") \
  OP(MUL_INT_LIT16     , Ref::Literal, "mul-int/lit16") \
  OP(DIV_INT_LIT16     , Ref::Literal, "div-int/lit16") \
  OP(REM_INT_LIT16     , Ref::Literal, "rem-int/lit16") \
  OP(AND_INT_LIT16     , Ref::Literal, "and-int/lit16") \
  OP(OR_INT_LIT16      , Ref::Literal, "or-int/lit16") \
  OP(XOR_INT_LIT16     , Ref::Literal, "xor-int/lit16") \
  OP(ADD_INT_LIT8      , Ref::Literal, "add-int/lit8") \
  OP(RSUB_INT_LIT8     , Ref::Literal, "rsub-int/lit8") \
  OP(MUL_INT_LIT8      , Ref::Literal, "mul-int/lit8") \
  OP(DIV_INT_LIT8      , Ref::Literal, "div-int/lit8") \
  OP(REM_INT_LIT8      , Ref::Literal, "rem-int/lit8") \
  OP(AND_INT_LIT8      , Ref::Literal, "and-int/lit8") \
  OP(OR_INT_LIT8       , Ref::Literal, "or-int/lit8") \
  OP(XOR_INT_LIT8      , Ref::Literal, "xor-int/lit8") \
  OP(SHL_INT_LIT8      , Ref::Literal, "shl-int/lit8") \
  OP(SHR_INT_LIT8      , Ref::Literal, "shr-int/lit8") \
  OP(USHR_INT_LIT8     , Ref::Literal, "ushr-int/lit8")

enum IROpcode : uint16_t {
#define OP(op, code, ...) OPCODE_##op,
  OPS
#undef OP
  IOPCODE_LOAD_PARAM,
  IOPCODE_LOAD_PARAM_OBJECT,
  IOPCODE_LOAD_PARAM_WIDE,

  IOPCODE_MOVE_RESULT_PSEUDO,
  IOPCODE_MOVE_RESULT_PSEUDO_OBJECT,
  IOPCODE_MOVE_RESULT_PSEUDO_WIDE,
};

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

// Internal opcodes cannot be mapped to a corresponding DexOpcode.
bool is_internal(IROpcode);

bool is_load_param(IROpcode);

bool is_move_result_pseudo(IROpcode);

bool is_move_result_or_move_result_pseudo(IROpcode op);

bool is_move(IROpcode);

IROpcode load_param_to_move(IROpcode);

IROpcode invert_conditional_branch(IROpcode op);

IROpcode move_result_pseudo_for_iget(IROpcode op);

IROpcode move_result_pseudo_for_sget(IROpcode op);

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

} // namespace opcode

/*
 * The functions below probably shouldn't be accessed directly by opt/ code.
 * They're implementation details wrapped by IRInstruction and DexInstruction.
 */
namespace opcode_impl {

unsigned dests_size(IROpcode);

bool has_move_result_pseudo(IROpcode);

// we can't tell the srcs size from the opcode alone -- format 35c opcodes
// encode that separately. So this just returns the minimum.
unsigned min_srcs_size(IROpcode);

bool dest_is_wide(IROpcode);

bool dest_is_object(IROpcode);

} // namespace opcode_impl
