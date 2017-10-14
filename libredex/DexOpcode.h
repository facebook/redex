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
  Literal,
  String,
  Type,
  Field,
  Method,
  Data,
};

} // namespace opcode

#define OPS \
  OP(NOP                    , 0x00, f10x, Ref::None, "nop") \
  OP(MOVE                   , 0x01, f12x, Ref::None, "move") \
  OP(MOVE_FROM16            , 0x02, f22x, Ref::None, "move/from16") \
  OP(MOVE_16                , 0x03, f32x, Ref::None, "move/16") \
  OP(MOVE_WIDE              , 0x04, f12x, Ref::None, "move-wide") \
  OP(MOVE_WIDE_FROM16       , 0x05, f22x, Ref::None, "move-wide/from16") \
  OP(MOVE_WIDE_16           , 0x06, f32x, Ref::None, "move-wide/16") \
  OP(MOVE_OBJECT            , 0x07, f12x, Ref::None, "move-object") \
  OP(MOVE_OBJECT_FROM16     , 0x08, f22x, Ref::None, "move-object/from16") \
  OP(MOVE_OBJECT_16         , 0x09, f32x, Ref::None, "move-object/16") \
  OP(MOVE_RESULT            , 0x0a, f11x_d, Ref::None, "move-result") \
  OP(MOVE_RESULT_WIDE       , 0x0b, f11x_d, Ref::None, "move-result-wide") \
  OP(MOVE_RESULT_OBJECT     , 0x0c, f11x_d, Ref::None, "move-result-object") \
  OP(MOVE_EXCEPTION         , 0x0d, f11x_d, Ref::None, "move-exception") \
  OP(RETURN_VOID            , 0x0e, f10x, Ref::None, "return-void") \
  OP(RETURN                 , 0x0f, f11x_s, Ref::None, "return") \
  OP(RETURN_WIDE            , 0x10, f11x_s, Ref::None, "return-wide") \
  OP(RETURN_OBJECT          , 0x11, f11x_s, Ref::None, "return-object") \
  OP(CONST_4                , 0x12, f11n, Ref::Literal, "const/4") \
  OP(CONST_16               , 0x13, f21s, Ref::Literal, "const/16") \
  OP(CONST                  , 0x14, f31i, Ref::Literal, "const") \
  OP(CONST_HIGH16           , 0x15, f21h, Ref::Literal, "const-high16") \
  OP(CONST_WIDE_16          , 0x16, f21s, Ref::Literal, "const-wide/16") \
  OP(CONST_WIDE_32          , 0x17, f31i, Ref::Literal, "const-wide-32") \
  OP(CONST_WIDE             , 0x18, f51l, Ref::Literal, "const-wide") \
  OP(CONST_WIDE_HIGH16      , 0x19, f21h, Ref::Literal, "const-wide-high16") \
  OP(CONST_STRING           , 0x1a, f21c_d, Ref::String, "const-string") \
  OP(CONST_STRING_JUMBO     , 0x1b, f31c, Ref::String, "const-string-jumbo") \
  OP(CONST_CLASS            , 0x1c, f21c_d, Ref::Type, "const-class") \
  OP(MONITOR_ENTER          , 0x1d, f11x_s, Ref::None, "monitor-enter") \
  OP(MONITOR_EXIT           , 0x1e, f11x_s, Ref::None, "monitor-exit") \
  OP(CHECK_CAST             , 0x1f, f21c_s, Ref::Type, "check-cast") \
  OP(INSTANCE_OF            , 0x20, f22c_d, Ref::Type, "instance-of") \
  OP(ARRAY_LENGTH           , 0x21, f12x, Ref::None, "array-length") \
  OP(NEW_INSTANCE           , 0x22, f21c_d, Ref::Type, "new-instance") \
  OP(NEW_ARRAY              , 0x23, f22c_d, Ref::Type, "new-array") \
  OP(FILLED_NEW_ARRAY       , 0x24, f35c, Ref::Type, "filled-new-array") \
  OP(FILLED_NEW_ARRAY_RANGE , 0x25, f3rc, Ref::Type, "filled-new-array-range") \
  OP(FILL_ARRAY_DATA        , 0x26, f31t, Ref::Data, "fill-array-data") \
  OP(THROW                  , 0x27, f11x_s, Ref::None, "throw") \
  OP(GOTO                   , 0x28, f10t, Ref::None, "goto") \
  OP(GOTO_16                , 0x29, f20t, Ref::None, "goto/16") \
  OP(GOTO_32                , 0x2a, f30t, Ref::None, "goto-32") \
  OP(PACKED_SWITCH          , 0x2b, f31t, Ref::None, "packed-switch") \
  OP(SPARSE_SWITCH          , 0x2c, f31t, Ref::None, "sparse-switch") \
  OP(CMPL_FLOAT             , 0x2d, f23x_d, Ref::None, "cmpl-float") \
  OP(CMPG_FLOAT             , 0x2e, f23x_d, Ref::None, "cmpg-float") \
  OP(CMPL_DOUBLE            , 0x2f, f23x_d, Ref::None, "cmpl-double") \
  OP(CMPG_DOUBLE            , 0x30, f23x_d, Ref::None, "cmpg-double") \
  OP(CMP_LONG               , 0x31, f23x_d, Ref::None, "cmp-long") \
  OP(IF_EQ                  , 0x32, f22t, Ref::None, "if-eq") \
  OP(IF_NE                  , 0x33, f22t, Ref::None, "if-ne") \
  OP(IF_LT                  , 0x34, f22t, Ref::None, "if-lt") \
  OP(IF_GE                  , 0x35, f22t, Ref::None, "if-ge") \
  OP(IF_GT                  , 0x36, f22t, Ref::None, "if-gt") \
  OP(IF_LE                  , 0x37, f22t, Ref::None, "if-le") \
  OP(IF_EQZ                 , 0x38, f21t, Ref::None, "if-eqz") \
  OP(IF_NEZ                 , 0x39, f21t, Ref::None, "if-nez") \
  OP(IF_LTZ                 , 0x3a, f21t, Ref::None, "if-ltz") \
  OP(IF_GEZ                 , 0x3b, f21t, Ref::None, "if-gez") \
  OP(IF_GTZ                 , 0x3c, f21t, Ref::None, "if-gtz") \
  OP(IF_LEZ                 , 0x3d, f21t, Ref::None, "if-lez") \
  OP(AGET                   , 0x44, f23x_d, Ref::None, "aget") \
  OP(AGET_WIDE              , 0x45, f23x_d, Ref::None, "aget-wide") \
  OP(AGET_OBJECT            , 0x46, f23x_d, Ref::None, "aget-object") \
  OP(AGET_BOOLEAN           , 0x47, f23x_d, Ref::None, "aget-boolean") \
  OP(AGET_BYTE              , 0x48, f23x_d, Ref::None, "aget-byte") \
  OP(AGET_CHAR              , 0x49, f23x_d, Ref::None, "aget-char") \
  OP(AGET_SHORT             , 0x4a, f23x_d, Ref::None, "aget-short") \
  OP(APUT                   , 0x4b, f23x_s, Ref::None, "aput") \
  OP(APUT_WIDE              , 0x4c, f23x_s, Ref::None, "aput-wide") \
  OP(APUT_OBJECT            , 0x4d, f23x_s, Ref::None, "aput-object") \
  OP(APUT_BOOLEAN           , 0x4e, f23x_s, Ref::None, "aput-boolean") \
  OP(APUT_BYTE              , 0x4f, f23x_s, Ref::None, "aput-byte") \
  OP(APUT_CHAR              , 0x50, f23x_s, Ref::None, "aput-char") \
  OP(APUT_SHORT             , 0x51, f23x_s, Ref::None, "aput-short") \
  OP(IGET                   , 0x52, f22c_d, Ref::Field, "iget") \
  OP(IGET_WIDE              , 0x53, f22c_d, Ref::Field, "iget-wide") \
  OP(IGET_OBJECT            , 0x54, f22c_d, Ref::Field, "iget-object") \
  OP(IGET_BOOLEAN           , 0x55, f22c_d, Ref::Field, "iget-boolean") \
  OP(IGET_BYTE              , 0x56, f22c_d, Ref::Field, "iget-byte") \
  OP(IGET_CHAR              , 0x57, f22c_d, Ref::Field, "iget-char") \
  OP(IGET_SHORT             , 0x58, f22c_d, Ref::Field, "iget-short") \
  OP(IPUT                   , 0x59, f22c_s, Ref::Field, "iput") \
  OP(IPUT_WIDE              , 0x5a, f22c_s, Ref::Field, "iput-wide") \
  OP(IPUT_OBJECT            , 0x5b, f22c_s, Ref::Field, "iput-object") \
  OP(IPUT_BOOLEAN           , 0x5c, f22c_s, Ref::Field, "iput-boolean") \
  OP(IPUT_BYTE              , 0x5d, f22c_s, Ref::Field, "iput-byte") \
  OP(IPUT_CHAR              , 0x5e, f22c_s, Ref::Field, "iput-char") \
  OP(IPUT_SHORT             , 0x5f, f22c_s, Ref::Field, "iput-short") \
  OP(SGET                   , 0x60, f21c_d, Ref::Field, "sget") \
  OP(SGET_WIDE              , 0x61, f21c_d, Ref::Field, "sget-wide") \
  OP(SGET_OBJECT            , 0x62, f21c_d, Ref::Field, "sget-object") \
  OP(SGET_BOOLEAN           , 0x63, f21c_d, Ref::Field, "sget-boolean") \
  OP(SGET_BYTE              , 0x64, f21c_d, Ref::Field, "sget-byte") \
  OP(SGET_CHAR              , 0x65, f21c_d, Ref::Field, "sget-char") \
  OP(SGET_SHORT             , 0x66, f21c_d, Ref::Field, "sget-short") \
  OP(SPUT                   , 0x67, f21c_s, Ref::Field, "sput") \
  OP(SPUT_WIDE              , 0x68, f21c_s, Ref::Field, "sput-wide") \
  OP(SPUT_OBJECT            , 0x69, f21c_s, Ref::Field, "sput-object") \
  OP(SPUT_BOOLEAN           , 0x6a, f21c_s, Ref::Field, "sput-boolean") \
  OP(SPUT_BYTE              , 0x6b, f21c_s, Ref::Field, "sput-byte") \
  OP(SPUT_CHAR              , 0x6c, f21c_s, Ref::Field, "sput-char") \
  OP(SPUT_SHORT             , 0x6d, f21c_s, Ref::Field, "sput-short") \
  OP(INVOKE_VIRTUAL         , 0x6e, f35c, Ref::Method, "invoke-virtual") \
  OP(INVOKE_SUPER           , 0x6f, f35c, Ref::Method, "invoke-super") \
  OP(INVOKE_DIRECT          , 0x70, f35c, Ref::Method, "invoke-direct") \
  OP(INVOKE_STATIC          , 0x71, f35c, Ref::Method, "invoke-static") \
  OP(INVOKE_INTERFACE       , 0x72, f35c, Ref::Method, "invoke-interface") \
  OP(INVOKE_VIRTUAL_RANGE   , 0x74, f3rc, Ref::Method, "invoke-virtual-range") \
  OP(INVOKE_SUPER_RANGE     , 0x75, f3rc, Ref::Method, "invoke-super-range") \
  OP(INVOKE_DIRECT_RANGE    , 0x76, f3rc, Ref::Method, "invoke-direct-range") \
  OP(INVOKE_STATIC_RANGE    , 0x77, f3rc, Ref::Method, "invoke-static-range") \
  OP(INVOKE_INTERFACE_RANGE , 0x78, f3rc, Ref::Method, "invoke-interface-range") \
  OP(NEG_INT                , 0x7b, f12x, Ref::None, "neg-int") \
  OP(NOT_INT                , 0x7c, f12x, Ref::None, "not-int") \
  OP(NEG_LONG               , 0x7d, f12x, Ref::None, "neg-long") \
  OP(NOT_LONG               , 0x7e, f12x, Ref::None, "not-long") \
  OP(NEG_FLOAT              , 0x7f, f12x, Ref::None, "neg-float") \
  OP(NEG_DOUBLE             , 0x80, f12x, Ref::None, "neg-double") \
  OP(INT_TO_LONG            , 0x81, f12x, Ref::None, "int-to-long") \
  OP(INT_TO_FLOAT           , 0x82, f12x, Ref::None, "int-to-float") \
  OP(INT_TO_DOUBLE          , 0x83, f12x, Ref::None, "int-to-double") \
  OP(LONG_TO_INT            , 0x84, f12x, Ref::None, "long-to-int") \
  OP(LONG_TO_FLOAT          , 0x85, f12x, Ref::None, "long-to-float") \
  OP(LONG_TO_DOUBLE         , 0x86, f12x, Ref::None, "long-to-double") \
  OP(FLOAT_TO_INT           , 0x87, f12x, Ref::None, "float-to-int") \
  OP(FLOAT_TO_LONG          , 0x88, f12x, Ref::None, "float-to-long") \
  OP(FLOAT_TO_DOUBLE        , 0x89, f12x, Ref::None, "float-to-double") \
  OP(DOUBLE_TO_INT          , 0x8a, f12x, Ref::None, "double-to-int") \
  OP(DOUBLE_TO_LONG         , 0x8b, f12x, Ref::None, "double-to-long") \
  OP(DOUBLE_TO_FLOAT        , 0x8c, f12x, Ref::None, "double-to-float") \
  OP(INT_TO_BYTE            , 0x8d, f12x, Ref::None, "int-to-byte") \
  OP(INT_TO_CHAR            , 0x8e, f12x, Ref::None, "int-to-char") \
  OP(INT_TO_SHORT           , 0x8f, f12x, Ref::None, "int-to-short") \
  OP(ADD_INT                , 0x90, f23x_d, Ref::None, "add-int") \
  OP(SUB_INT                , 0x91, f23x_d, Ref::None, "sub-int") \
  OP(MUL_INT                , 0x92, f23x_d, Ref::None, "mul-int") \
  OP(DIV_INT                , 0x93, f23x_d, Ref::None, "div-int") \
  OP(REM_INT                , 0x94, f23x_d, Ref::None, "rem-int") \
  OP(AND_INT                , 0x95, f23x_d, Ref::None, "and-int") \
  OP(OR_INT                 , 0x96, f23x_d, Ref::None, "or-int") \
  OP(XOR_INT                , 0x97, f23x_d, Ref::None, "xor-int") \
  OP(SHL_INT                , 0x98, f23x_d, Ref::None, "shl-int") \
  OP(SHR_INT                , 0x99, f23x_d, Ref::None, "shr-int") \
  OP(USHR_INT               , 0x9a, f23x_d, Ref::None, "ushr-int") \
  OP(ADD_LONG               , 0x9b, f23x_d, Ref::None, "add-long") \
  OP(SUB_LONG               , 0x9c, f23x_d, Ref::None, "sub-long") \
  OP(MUL_LONG               , 0x9d, f23x_d, Ref::None, "mul-long") \
  OP(DIV_LONG               , 0x9e, f23x_d, Ref::None, "div-long") \
  OP(REM_LONG               , 0x9f, f23x_d, Ref::None, "rem-long") \
  OP(AND_LONG               , 0xa0, f23x_d, Ref::None, "and-long") \
  OP(OR_LONG                , 0xa1, f23x_d, Ref::None, "or-long") \
  OP(XOR_LONG               , 0xa2, f23x_d, Ref::None, "xor-long") \
  OP(SHL_LONG               , 0xa3, f23x_d, Ref::None, "shl-long") \
  OP(SHR_LONG               , 0xa4, f23x_d, Ref::None, "shr-long") \
  OP(USHR_LONG              , 0xa5, f23x_d, Ref::None, "ushr-long") \
  OP(ADD_FLOAT              , 0xa6, f23x_d, Ref::None, "add-float") \
  OP(SUB_FLOAT              , 0xa7, f23x_d, Ref::None, "sub-float") \
  OP(MUL_FLOAT              , 0xa8, f23x_d, Ref::None, "mul-float") \
  OP(DIV_FLOAT              , 0xa9, f23x_d, Ref::None, "div-float") \
  OP(REM_FLOAT              , 0xaa, f23x_d, Ref::None, "rem-float") \
  OP(ADD_DOUBLE             , 0xab, f23x_d, Ref::None, "add-double") \
  OP(SUB_DOUBLE             , 0xac, f23x_d, Ref::None, "sub-double") \
  OP(MUL_DOUBLE             , 0xad, f23x_d, Ref::None, "mul-double") \
  OP(DIV_DOUBLE             , 0xae, f23x_d, Ref::None, "div-double") \
  OP(REM_DOUBLE             , 0xaf, f23x_d, Ref::None, "rem-double") \
  OP(ADD_INT_2ADDR          , 0xb0, f12x_2, Ref::None, "add-int/2addr") \
  OP(SUB_INT_2ADDR          , 0xb1, f12x_2, Ref::None, "sub-int/2addr") \
  OP(MUL_INT_2ADDR          , 0xb2, f12x_2, Ref::None, "mul-int/2addr") \
  OP(DIV_INT_2ADDR          , 0xb3, f12x_2, Ref::None, "div-int/2addr") \
  OP(REM_INT_2ADDR          , 0xb4, f12x_2, Ref::None, "rem-int/2addr") \
  OP(AND_INT_2ADDR          , 0xb5, f12x_2, Ref::None, "and-int/2addr") \
  OP(OR_INT_2ADDR           , 0xb6, f12x_2, Ref::None, "or-int/2addr") \
  OP(XOR_INT_2ADDR          , 0xb7, f12x_2, Ref::None, "xor-int/2addr") \
  OP(SHL_INT_2ADDR          , 0xb8, f12x_2, Ref::None, "shl-int/2addr") \
  OP(SHR_INT_2ADDR          , 0xb9, f12x_2, Ref::None, "shr-int/2addr") \
  OP(USHR_INT_2ADDR         , 0xba, f12x_2, Ref::None, "ushr-int/2addr") \
  OP(ADD_LONG_2ADDR         , 0xbb, f12x_2, Ref::None, "add-long/2addr") \
  OP(SUB_LONG_2ADDR         , 0xbc, f12x_2, Ref::None, "sub-long/2addr") \
  OP(MUL_LONG_2ADDR         , 0xbd, f12x_2, Ref::None, "mul-long/2addr") \
  OP(DIV_LONG_2ADDR         , 0xbe, f12x_2, Ref::None, "div-long/2addr") \
  OP(REM_LONG_2ADDR         , 0xbf, f12x_2, Ref::None, "rem-long/2addr") \
  OP(AND_LONG_2ADDR         , 0xc0, f12x_2, Ref::None, "and-long/2addr") \
  OP(OR_LONG_2ADDR          , 0xc1, f12x_2, Ref::None, "or-long/2addr") \
  OP(XOR_LONG_2ADDR         , 0xc2, f12x_2, Ref::None, "xor-long/2addr") \
  OP(SHL_LONG_2ADDR         , 0xc3, f12x_2, Ref::None, "shl-long/2addr") \
  OP(SHR_LONG_2ADDR         , 0xc4, f12x_2, Ref::None, "shr-long/2addr") \
  OP(USHR_LONG_2ADDR        , 0xc5, f12x_2, Ref::None, "ushr-long/2addr") \
  OP(ADD_FLOAT_2ADDR        , 0xc6, f12x_2, Ref::None, "add-float/2addr") \
  OP(SUB_FLOAT_2ADDR        , 0xc7, f12x_2, Ref::None, "sub-float/2addr") \
  OP(MUL_FLOAT_2ADDR        , 0xc8, f12x_2, Ref::None, "mul-float/2addr") \
  OP(DIV_FLOAT_2ADDR        , 0xc9, f12x_2, Ref::None, "div-float/2addr") \
  OP(REM_FLOAT_2ADDR        , 0xca, f12x_2, Ref::None, "rem-float/2addr") \
  OP(ADD_DOUBLE_2ADDR       , 0xcb, f12x_2, Ref::None, "add-double/2addr") \
  OP(SUB_DOUBLE_2ADDR       , 0xcc, f12x_2, Ref::None, "sub-double/2addr") \
  OP(MUL_DOUBLE_2ADDR       , 0xcd, f12x_2, Ref::None, "mul-double/2addr") \
  OP(DIV_DOUBLE_2ADDR       , 0xce, f12x_2, Ref::None, "div-double/2addr") \
  OP(REM_DOUBLE_2ADDR       , 0xcf, f12x_2, Ref::None, "rem-double/2addr") \
  OP(ADD_INT_LIT16          , 0xd0, f22s, Ref::Literal, "add-int/lit16") \
  OP(RSUB_INT               , 0xd1, f22s, Ref::Literal, "rsub-int") \
  OP(MUL_INT_LIT16          , 0xd2, f22s, Ref::Literal, "mul-int/lit16") \
  OP(DIV_INT_LIT16          , 0xd3, f22s, Ref::Literal, "div-int/lit16") \
  OP(REM_INT_LIT16          , 0xd4, f22s, Ref::Literal, "rem-int/lit16") \
  OP(AND_INT_LIT16          , 0xd5, f22s, Ref::Literal, "and-int/lit16") \
  OP(OR_INT_LIT16           , 0xd6, f22s, Ref::Literal, "or-int/lit16") \
  OP(XOR_INT_LIT16          , 0xd7, f22s, Ref::Literal, "xor-int/lit16") \
  OP(ADD_INT_LIT8           , 0xd8, f22b, Ref::Literal, "add-int/lit8") \
  OP(RSUB_INT_LIT8          , 0xd9, f22b, Ref::Literal, "rsub-int/lit8") \
  OP(MUL_INT_LIT8           , 0xda, f22b, Ref::Literal, "mul-int/lit8") \
  OP(DIV_INT_LIT8           , 0xdb, f22b, Ref::Literal, "div-int/lit8") \
  OP(REM_INT_LIT8           , 0xdc, f22b, Ref::Literal, "rem-int/lit8") \
  OP(AND_INT_LIT8           , 0xdd, f22b, Ref::Literal, "and-int/lit8") \
  OP(OR_INT_LIT8            , 0xde, f22b, Ref::Literal, "or-int/lit8") \
  OP(XOR_INT_LIT8           , 0xdf, f22b, Ref::Literal, "xor-int/lit8") \
  OP(SHL_INT_LIT8           , 0xe0, f22b, Ref::Literal, "shl-int/lit8") \
  OP(SHR_INT_LIT8           , 0xe1, f22b, Ref::Literal, "shr-int/lit8") \
  OP(USHR_INT_LIT8          , 0xe2, f22b, Ref::Literal, "ushr-int/lit8")

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

  IOPCODE_MOVE_RESULT_PSEUDO = 0xf300,
  IOPCODE_MOVE_RESULT_PSEUDO_OBJECT = 0xf400,
  IOPCODE_MOVE_RESULT_PSEUDO_WIDE = 0xf500,
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

bool is_internal(DexOpcode);

bool is_load_param(DexOpcode);

bool is_move_result_pseudo(DexOpcode);

DexOpcode move_result_pseudo_for_iget(DexOpcode op);
DexOpcode move_result_pseudo_for_sget(DexOpcode op);

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

bool dest_is_wide(DexOpcode);

bool dest_is_object(DexOpcode);

// the number of bits an opcode has available to encode a given register
bit_width_t src_bit_width(DexOpcode, uint16_t i);
bit_width_t dest_bit_width(DexOpcode);

} // namespace opcode_impl
