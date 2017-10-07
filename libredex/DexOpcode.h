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
#include <string>

/*
 * Dex opcode formats as defined by the spec; the _d and _s variants indicate
 * whether the first register parameter is a destination or source register.
 */
enum DexOpcodeFormat : uint8_t {
  FMT_f00x,
  FMT_f10x,
  FMT_f12x,
  FMT_f12x_2,
  FMT_f11n,
  FMT_f11x_d,
  FMT_f11x_s,
  FMT_f10t,
  FMT_f20t,
  FMT_f20bc,
  FMT_f22x,
  FMT_f21t,
  FMT_f21s,
  FMT_f21h,
  FMT_f21c_d,
  FMT_f21c_s,
  FMT_f23x_d,
  FMT_f23x_s,
  FMT_f22b,
  FMT_f22t,
  FMT_f22s,
  FMT_f22c_d,
  FMT_f22c_s,
  FMT_f22cs,
  FMT_f30t,
  FMT_f32x,
  FMT_f31i,
  FMT_f31t,
  FMT_f31c,
  FMT_f35c,
  FMT_f35ms,
  FMT_f35mi,
  FMT_f3rc,
  FMT_f3rms,
  FMT_f3rmi,
  FMT_f51l,
  FMT_f41c_d,
  FMT_f41c_s,
  FMT_f52c_d,
  FMT_f52c_s,
  FMT_f5rc,
  FMT_f57c,
  FMT_fopcode,
  FMT_iopcode,
};

namespace opcode {

enum class Ref {
  None,
  String,
  Type,
  Field,
  Method,
  Data,
};

} // namespace opcode

#define OPS                                                     \
  OP(NOP                          , 0x00, f10x, Ref::None)      \
  OP(MOVE                         , 0x01, f12x, Ref::None)      \
  OP(MOVE_FROM16                  , 0x02, f22x, Ref::None)      \
  OP(MOVE_16                      , 0x03, f32x, Ref::None)      \
  OP(MOVE_WIDE                    , 0x04, f12x, Ref::None)      \
  OP(MOVE_WIDE_FROM16             , 0x05, f22x, Ref::None)      \
  OP(MOVE_WIDE_16                 , 0x06, f32x, Ref::None)      \
  OP(MOVE_OBJECT                  , 0x07, f12x, Ref::None)      \
  OP(MOVE_OBJECT_FROM16           , 0x08, f22x, Ref::None)      \
  OP(MOVE_OBJECT_16               , 0x09, f32x, Ref::None)      \
  OP(MOVE_RESULT                  , 0x0a, f11x_d, Ref::None)    \
  OP(MOVE_RESULT_WIDE             , 0x0b, f11x_d, Ref::None)    \
  OP(MOVE_RESULT_OBJECT           , 0x0c, f11x_d, Ref::None)    \
  OP(MOVE_EXCEPTION               , 0x0d, f11x_d, Ref::None)    \
  OP(RETURN_VOID                  , 0x0e, f10x, Ref::None)      \
  OP(RETURN                       , 0x0f, f11x_s, Ref::None)    \
  OP(RETURN_WIDE                  , 0x10, f11x_s, Ref::None)    \
  OP(RETURN_OBJECT                , 0x11, f11x_s, Ref::None)    \
  OP(CONST_4                      , 0x12, f11n, Ref::None)      \
  OP(CONST_16                     , 0x13, f21s, Ref::None)      \
  OP(CONST                        , 0x14, f31i, Ref::None)      \
  OP(CONST_HIGH16                 , 0x15, f21h, Ref::None)      \
  OP(CONST_WIDE_16                , 0x16, f21s, Ref::None)      \
  OP(CONST_WIDE_32                , 0x17, f31i, Ref::None)      \
  OP(CONST_WIDE                   , 0x18, f51l, Ref::None)      \
  OP(CONST_WIDE_HIGH16            , 0x19, f21h, Ref::None)      \
  OP(CONST_STRING                 , 0x1a, f21c_d, Ref::String)  \
  OP(CONST_STRING_JUMBO           , 0x1b, f31c, Ref::String)    \
  OP(CONST_CLASS                  , 0x1c, f21c_d, Ref::Type)    \
  OP(MONITOR_ENTER                , 0x1d, f11x_s, Ref::None)    \
  OP(MONITOR_EXIT                 , 0x1e, f11x_s, Ref::None)    \
  OP(CHECK_CAST                   , 0x1f, f21c_s, Ref::Type)    \
  OP(INSTANCE_OF                  , 0x20, f22c_d, Ref::Type)    \
  OP(ARRAY_LENGTH                 , 0x21, f12x, Ref::None)      \
  OP(NEW_INSTANCE                 , 0x22, f21c_d, Ref::Type)    \
  OP(NEW_ARRAY                    , 0x23, f22c_d, Ref::Type)    \
  OP(FILLED_NEW_ARRAY             , 0x24, f35c, Ref::Type)      \
  OP(FILLED_NEW_ARRAY_RANGE       , 0x25, f3rc, Ref::Type)      \
  OP(FILL_ARRAY_DATA              , 0x26, f31t, Ref::Data)      \
  OP(THROW                        , 0x27, f11x_s, Ref::None)    \
  OP(GOTO                         , 0x28, f10t, Ref::None)      \
  OP(GOTO_16                      , 0x29, f20t, Ref::None)      \
  OP(GOTO_32                      , 0x2a, f30t, Ref::None)      \
  OP(PACKED_SWITCH                , 0x2b, f31t, Ref::None)      \
  OP(SPARSE_SWITCH                , 0x2c, f31t, Ref::None)      \
  OP(CMPL_FLOAT                   , 0x2d, f23x_d, Ref::None)    \
  OP(CMPG_FLOAT                   , 0x2e, f23x_d, Ref::None)    \
  OP(CMPL_DOUBLE                  , 0x2f, f23x_d, Ref::None)    \
  OP(CMPG_DOUBLE                  , 0x30, f23x_d, Ref::None)    \
  OP(CMP_LONG                     , 0x31, f23x_d, Ref::None)    \
  OP(IF_EQ                        , 0x32, f22t, Ref::None)      \
  OP(IF_NE                        , 0x33, f22t, Ref::None)      \
  OP(IF_LT                        , 0x34, f22t, Ref::None)      \
  OP(IF_GE                        , 0x35, f22t, Ref::None)      \
  OP(IF_GT                        , 0x36, f22t, Ref::None)      \
  OP(IF_LE                        , 0x37, f22t, Ref::None)      \
  OP(IF_EQZ                       , 0x38, f21t, Ref::None)      \
  OP(IF_NEZ                       , 0x39, f21t, Ref::None)      \
  OP(IF_LTZ                       , 0x3a, f21t, Ref::None)      \
  OP(IF_GEZ                       , 0x3b, f21t, Ref::None)      \
  OP(IF_GTZ                       , 0x3c, f21t, Ref::None)      \
  OP(IF_LEZ                       , 0x3d, f21t, Ref::None)      \
  OP(AGET                         , 0x44, f23x_d, Ref::None)    \
  OP(AGET_WIDE                    , 0x45, f23x_d, Ref::None)    \
  OP(AGET_OBJECT                  , 0x46, f23x_d, Ref::None)    \
  OP(AGET_BOOLEAN                 , 0x47, f23x_d, Ref::None)    \
  OP(AGET_BYTE                    , 0x48, f23x_d, Ref::None)    \
  OP(AGET_CHAR                    , 0x49, f23x_d, Ref::None)    \
  OP(AGET_SHORT                   , 0x4a, f23x_d, Ref::None)    \
  OP(APUT                         , 0x4b, f23x_s, Ref::None)    \
  OP(APUT_WIDE                    , 0x4c, f23x_s, Ref::None)    \
  OP(APUT_OBJECT                  , 0x4d, f23x_s, Ref::None)    \
  OP(APUT_BOOLEAN                 , 0x4e, f23x_s, Ref::None)    \
  OP(APUT_BYTE                    , 0x4f, f23x_s, Ref::None)    \
  OP(APUT_CHAR                    , 0x50, f23x_s, Ref::None)    \
  OP(APUT_SHORT                   , 0x51, f23x_s, Ref::None)    \
  OP(IGET                         , 0x52, f22c_d, Ref::Field)   \
  OP(IGET_WIDE                    , 0x53, f22c_d, Ref::Field)   \
  OP(IGET_OBJECT                  , 0x54, f22c_d, Ref::Field)   \
  OP(IGET_BOOLEAN                 , 0x55, f22c_d, Ref::Field)   \
  OP(IGET_BYTE                    , 0x56, f22c_d, Ref::Field)   \
  OP(IGET_CHAR                    , 0x57, f22c_d, Ref::Field)   \
  OP(IGET_SHORT                   , 0x58, f22c_d, Ref::Field)   \
  OP(IPUT                         , 0x59, f22c_s, Ref::Field)   \
  OP(IPUT_WIDE                    , 0x5a, f22c_s, Ref::Field)   \
  OP(IPUT_OBJECT                  , 0x5b, f22c_s, Ref::Field)   \
  OP(IPUT_BOOLEAN                 , 0x5c, f22c_s, Ref::Field)   \
  OP(IPUT_BYTE                    , 0x5d, f22c_s, Ref::Field)   \
  OP(IPUT_CHAR                    , 0x5e, f22c_s, Ref::Field)   \
  OP(IPUT_SHORT                   , 0x5f, f22c_s, Ref::Field)   \
  OP(SGET                         , 0x60, f21c_d, Ref::Field)   \
  OP(SGET_WIDE                    , 0x61, f21c_d, Ref::Field)   \
  OP(SGET_OBJECT                  , 0x62, f21c_d, Ref::Field)   \
  OP(SGET_BOOLEAN                 , 0x63, f21c_d, Ref::Field)   \
  OP(SGET_BYTE                    , 0x64, f21c_d, Ref::Field)   \
  OP(SGET_CHAR                    , 0x65, f21c_d, Ref::Field)   \
  OP(SGET_SHORT                   , 0x66, f21c_d, Ref::Field)   \
  OP(SPUT                         , 0x67, f21c_s, Ref::Field)   \
  OP(SPUT_WIDE                    , 0x68, f21c_s, Ref::Field)   \
  OP(SPUT_OBJECT                  , 0x69, f21c_s, Ref::Field)   \
  OP(SPUT_BOOLEAN                 , 0x6a, f21c_s, Ref::Field)   \
  OP(SPUT_BYTE                    , 0x6b, f21c_s, Ref::Field)   \
  OP(SPUT_CHAR                    , 0x6c, f21c_s, Ref::Field)   \
  OP(SPUT_SHORT                   , 0x6d, f21c_s, Ref::Field)   \
  OP(INVOKE_VIRTUAL               , 0x6e, f35c, Ref::Method)    \
  OP(INVOKE_SUPER                 , 0x6f, f35c, Ref::Method)    \
  OP(INVOKE_DIRECT                , 0x70, f35c, Ref::Method)    \
  OP(INVOKE_STATIC                , 0x71, f35c, Ref::Method)    \
  OP(INVOKE_INTERFACE             , 0x72, f35c, Ref::Method)    \
  OP(INVOKE_VIRTUAL_RANGE         , 0x74, f3rc, Ref::Method)    \
  OP(INVOKE_SUPER_RANGE           , 0x75, f3rc, Ref::Method)    \
  OP(INVOKE_DIRECT_RANGE          , 0x76, f3rc, Ref::Method)    \
  OP(INVOKE_STATIC_RANGE          , 0x77, f3rc, Ref::Method)    \
  OP(INVOKE_INTERFACE_RANGE       , 0x78, f3rc, Ref::Method)    \
  OP(NEG_INT                      , 0x7b, f12x, Ref::None)      \
  OP(NOT_INT                      , 0x7c, f12x, Ref::None)      \
  OP(NEG_LONG                     , 0x7d, f12x, Ref::None)      \
  OP(NOT_LONG                     , 0x7e, f12x, Ref::None)      \
  OP(NEG_FLOAT                    , 0x7f, f12x, Ref::None)      \
  OP(NEG_DOUBLE                   , 0x80, f12x, Ref::None)      \
  OP(INT_TO_LONG                  , 0x81, f12x, Ref::None)      \
  OP(INT_TO_FLOAT                 , 0x82, f12x, Ref::None)      \
  OP(INT_TO_DOUBLE                , 0x83, f12x, Ref::None)      \
  OP(LONG_TO_INT                  , 0x84, f12x, Ref::None)      \
  OP(LONG_TO_FLOAT                , 0x85, f12x, Ref::None)      \
  OP(LONG_TO_DOUBLE               , 0x86, f12x, Ref::None)      \
  OP(FLOAT_TO_INT                 , 0x87, f12x, Ref::None)      \
  OP(FLOAT_TO_LONG                , 0x88, f12x, Ref::None)      \
  OP(FLOAT_TO_DOUBLE              , 0x89, f12x, Ref::None)      \
  OP(DOUBLE_TO_INT                , 0x8a, f12x, Ref::None)      \
  OP(DOUBLE_TO_LONG               , 0x8b, f12x, Ref::None)      \
  OP(DOUBLE_TO_FLOAT              , 0x8c, f12x, Ref::None)      \
  OP(INT_TO_BYTE                  , 0x8d, f12x, Ref::None)      \
  OP(INT_TO_CHAR                  , 0x8e, f12x, Ref::None)      \
  OP(INT_TO_SHORT                 , 0x8f, f12x, Ref::None)      \
  OP(ADD_INT                      , 0x90, f23x_d, Ref::None)    \
  OP(SUB_INT                      , 0x91, f23x_d, Ref::None)    \
  OP(MUL_INT                      , 0x92, f23x_d, Ref::None)    \
  OP(DIV_INT                      , 0x93, f23x_d, Ref::None)    \
  OP(REM_INT                      , 0x94, f23x_d, Ref::None)    \
  OP(AND_INT                      , 0x95, f23x_d, Ref::None)    \
  OP(OR_INT                       , 0x96, f23x_d, Ref::None)    \
  OP(XOR_INT                      , 0x97, f23x_d, Ref::None)    \
  OP(SHL_INT                      , 0x98, f23x_d, Ref::None)    \
  OP(SHR_INT                      , 0x99, f23x_d, Ref::None)    \
  OP(USHR_INT                     , 0x9a, f23x_d, Ref::None)    \
  OP(ADD_LONG                     , 0x9b, f23x_d, Ref::None)    \
  OP(SUB_LONG                     , 0x9c, f23x_d, Ref::None)    \
  OP(MUL_LONG                     , 0x9d, f23x_d, Ref::None)    \
  OP(DIV_LONG                     , 0x9e, f23x_d, Ref::None)    \
  OP(REM_LONG                     , 0x9f, f23x_d, Ref::None)    \
  OP(AND_LONG                     , 0xa0, f23x_d, Ref::None)    \
  OP(OR_LONG                      , 0xa1, f23x_d, Ref::None)    \
  OP(XOR_LONG                     , 0xa2, f23x_d, Ref::None)    \
  OP(SHL_LONG                     , 0xa3, f23x_d, Ref::None)    \
  OP(SHR_LONG                     , 0xa4, f23x_d, Ref::None)    \
  OP(USHR_LONG                    , 0xa5, f23x_d, Ref::None)    \
  OP(ADD_FLOAT                    , 0xa6, f23x_d, Ref::None)    \
  OP(SUB_FLOAT                    , 0xa7, f23x_d, Ref::None)    \
  OP(MUL_FLOAT                    , 0xa8, f23x_d, Ref::None)    \
  OP(DIV_FLOAT                    , 0xa9, f23x_d, Ref::None)    \
  OP(REM_FLOAT                    , 0xaa, f23x_d, Ref::None)    \
  OP(ADD_DOUBLE                   , 0xab, f23x_d, Ref::None)    \
  OP(SUB_DOUBLE                   , 0xac, f23x_d, Ref::None)    \
  OP(MUL_DOUBLE                   , 0xad, f23x_d, Ref::None)    \
  OP(DIV_DOUBLE                   , 0xae, f23x_d, Ref::None)    \
  OP(REM_DOUBLE                   , 0xaf, f23x_d, Ref::None)    \
  OP(ADD_INT_2ADDR                , 0xb0, f12x_2, Ref::None)    \
  OP(SUB_INT_2ADDR                , 0xb1, f12x_2, Ref::None)    \
  OP(MUL_INT_2ADDR                , 0xb2, f12x_2, Ref::None)    \
  OP(DIV_INT_2ADDR                , 0xb3, f12x_2, Ref::None)    \
  OP(REM_INT_2ADDR                , 0xb4, f12x_2, Ref::None)    \
  OP(AND_INT_2ADDR                , 0xb5, f12x_2, Ref::None)    \
  OP(OR_INT_2ADDR                 , 0xb6, f12x_2, Ref::None)    \
  OP(XOR_INT_2ADDR                , 0xb7, f12x_2, Ref::None)    \
  OP(SHL_INT_2ADDR                , 0xb8, f12x_2, Ref::None)    \
  OP(SHR_INT_2ADDR                , 0xb9, f12x_2, Ref::None)    \
  OP(USHR_INT_2ADDR               , 0xba, f12x_2, Ref::None)    \
  OP(ADD_LONG_2ADDR               , 0xbb, f12x_2, Ref::None)    \
  OP(SUB_LONG_2ADDR               , 0xbc, f12x_2, Ref::None)    \
  OP(MUL_LONG_2ADDR               , 0xbd, f12x_2, Ref::None)    \
  OP(DIV_LONG_2ADDR               , 0xbe, f12x_2, Ref::None)    \
  OP(REM_LONG_2ADDR               , 0xbf, f12x_2, Ref::None)    \
  OP(AND_LONG_2ADDR               , 0xc0, f12x_2, Ref::None)    \
  OP(OR_LONG_2ADDR                , 0xc1, f12x_2, Ref::None)    \
  OP(XOR_LONG_2ADDR               , 0xc2, f12x_2, Ref::None)    \
  OP(SHL_LONG_2ADDR               , 0xc3, f12x_2, Ref::None)    \
  OP(SHR_LONG_2ADDR               , 0xc4, f12x_2, Ref::None)    \
  OP(USHR_LONG_2ADDR              , 0xc5, f12x_2, Ref::None)    \
  OP(ADD_FLOAT_2ADDR              , 0xc6, f12x_2, Ref::None)    \
  OP(SUB_FLOAT_2ADDR              , 0xc7, f12x_2, Ref::None)    \
  OP(MUL_FLOAT_2ADDR              , 0xc8, f12x_2, Ref::None)    \
  OP(DIV_FLOAT_2ADDR              , 0xc9, f12x_2, Ref::None)    \
  OP(REM_FLOAT_2ADDR              , 0xca, f12x_2, Ref::None)    \
  OP(ADD_DOUBLE_2ADDR             , 0xcb, f12x_2, Ref::None)    \
  OP(SUB_DOUBLE_2ADDR             , 0xcc, f12x_2, Ref::None)    \
  OP(MUL_DOUBLE_2ADDR             , 0xcd, f12x_2, Ref::None)    \
  OP(DIV_DOUBLE_2ADDR             , 0xce, f12x_2, Ref::None)    \
  OP(REM_DOUBLE_2ADDR             , 0xcf, f12x_2, Ref::None)    \
  OP(ADD_INT_LIT16                , 0xd0, f22s, Ref::None)      \
  OP(RSUB_INT                     , 0xd1, f22s, Ref::None)      \
  OP(MUL_INT_LIT16                , 0xd2, f22s, Ref::None)      \
  OP(DIV_INT_LIT16                , 0xd3, f22s, Ref::None)      \
  OP(REM_INT_LIT16                , 0xd4, f22s, Ref::None)      \
  OP(AND_INT_LIT16                , 0xd5, f22s, Ref::None)      \
  OP(OR_INT_LIT16                 , 0xd6, f22s, Ref::None)      \
  OP(XOR_INT_LIT16                , 0xd7, f22s, Ref::None)      \
  OP(ADD_INT_LIT8                 , 0xd8, f22b, Ref::None)      \
  OP(RSUB_INT_LIT8                , 0xd9, f22b, Ref::None)      \
  OP(MUL_INT_LIT8                 , 0xda, f22b, Ref::None)      \
  OP(DIV_INT_LIT8                 , 0xdb, f22b, Ref::None)      \
  OP(REM_INT_LIT8                 , 0xdc, f22b, Ref::None)      \
  OP(AND_INT_LIT8                 , 0xdd, f22b, Ref::None)      \
  OP(OR_INT_LIT8                  , 0xde, f22b, Ref::None)      \
  OP(XOR_INT_LIT8                 , 0xdf, f22b, Ref::None)      \
  OP(SHL_INT_LIT8                 , 0xe0, f22b, Ref::None)      \
  OP(SHR_INT_LIT8                 , 0xe1, f22b, Ref::None)      \
  OP(USHR_INT_LIT8                , 0xe2, f22b, Ref::None)

enum DexOpcode : uint16_t {
#define OP(op, code, ...) OPCODE_ ## op = code,
OPS
#undef OP
  FOPCODE_PACKED_SWITCH = 0x0100,
  FOPCODE_SPARSE_SWITCH = 0x0200,
  FOPCODE_FILLED_ARRAY  = 0x0300,

  // used only by our IR
  IOPCODE_LOAD_PARAM = 0xf000,
  IOPCODE_LOAD_PARAM_OBJECT = 0xf100,
  IOPCODE_LOAD_PARAM_WIDE = 0xf200,
};

std::string show(DexOpcode);

using bit_width_t = uint8_t;

namespace opcode {

// max number of register args supported by non-range opcodes
const size_t NON_RANGE_MAX = 5;

DexOpcodeFormat format(DexOpcode opcode);

Ref ref(DexOpcode);

// if a source register is used as a destination too
bool dest_is_src(DexOpcode);
bool has_literal(DexOpcode);
bool has_offset(DexOpcode);
bool has_range(DexOpcode);

bool may_throw(DexOpcode);

// if an opcode has a /range counterpart
bool has_range_form(DexOpcode);

bool is_commutative(DexOpcode op);

bool is_load_param(DexOpcode);

} // namespace opcode

/*
 * The functions below probably shouldn't be accessed directly by opt/ code.
 * They're implementation details wrapped by IRInstruction and DexInstruction.
 */
namespace opcode_impl {

unsigned dests_size(DexOpcode);
// we can't tell the srcs size from the opcode alone -- format 35c opcodes
// encode that separately. So this just returns the minimum.
unsigned min_srcs_size(DexOpcode);

// the number of bits an opcode has available to encode a given register
bit_width_t src_bit_width(DexOpcode, uint16_t i);
bit_width_t dest_bit_width(DexOpcode);

} // namespace opcode_impl
