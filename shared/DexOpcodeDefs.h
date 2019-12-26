/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <stdint.h>
#include <string>

/*
 * Dex opcode formats as defined by the spec; the _d and _s variants indicate
 * whether the first register parameter is a destination or source register.
 */
enum OpcodeFormat : uint8_t {
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
  FMT_f45cc,
  FMT_f4rcc,
  FMT_f52c_d,
  FMT_f52c_s,
  FMT_f5rc,
  FMT_f57c,
  FMT_fopcode,
  FMT_iopcode,
};

#define DOPS                                                            \
  OP(NOP, 0x00, f10x, "nop")                                            \
  OP(MOVE, 0x01, f12x, "move")                                          \
  OP(MOVE_FROM16, 0x02, f22x, "move/from16")                            \
  OP(MOVE_16, 0x03, f32x, "move/16")                                    \
  OP(MOVE_WIDE, 0x04, f12x, "move-wide")                                \
  OP(MOVE_WIDE_FROM16, 0x05, f22x, "move-wide/from16")                  \
  OP(MOVE_WIDE_16, 0x06, f32x, "move-wide/16")                          \
  OP(MOVE_OBJECT, 0x07, f12x, "move-object")                            \
  OP(MOVE_OBJECT_FROM16, 0x08, f22x, "move-object/from16")              \
  OP(MOVE_OBJECT_16, 0x09, f32x, "move-object/16")                      \
  OP(MOVE_RESULT, 0x0a, f11x_d, "move-result")                          \
  OP(MOVE_RESULT_WIDE, 0x0b, f11x_d, "move-result-wide")                \
  OP(MOVE_RESULT_OBJECT, 0x0c, f11x_d, "move-result-object")            \
  OP(MOVE_EXCEPTION, 0x0d, f11x_d, "move-exception")                    \
  OP(RETURN_VOID, 0x0e, f10x, "return-void")                            \
  OP(RETURN, 0x0f, f11x_s, "return")                                    \
  OP(RETURN_WIDE, 0x10, f11x_s, "return-wide")                          \
  OP(RETURN_OBJECT, 0x11, f11x_s, "return-object")                      \
  OP(CONST_4, 0x12, f11n, "const/4")                                    \
  OP(CONST_16, 0x13, f21s, "const/16")                                  \
  OP(CONST, 0x14, f31i, "const")                                        \
  OP(CONST_HIGH16, 0x15, f21h, "const-high16")                          \
  OP(CONST_WIDE_16, 0x16, f21s, "const-wide/16")                        \
  OP(CONST_WIDE_32, 0x17, f31i, "const-wide-32")                        \
  OP(CONST_WIDE, 0x18, f51l, "const-wide")                              \
  OP(CONST_WIDE_HIGH16, 0x19, f21h, "const-wide-high16")                \
  OP(CONST_STRING, 0x1a, f21c_d, "const-string")                        \
  OP(CONST_STRING_JUMBO, 0x1b, f31c, "const-string-jumbo")              \
  OP(CONST_CLASS, 0x1c, f21c_d, "const-class")                          \
  OP(MONITOR_ENTER, 0x1d, f11x_s, "monitor-enter")                      \
  OP(MONITOR_EXIT, 0x1e, f11x_s, "monitor-exit")                        \
  OP(CHECK_CAST, 0x1f, f21c_s, "check-cast")                            \
  OP(INSTANCE_OF, 0x20, f22c_d, "instance-of")                          \
  OP(ARRAY_LENGTH, 0x21, f12x, "array-length")                          \
  OP(NEW_INSTANCE, 0x22, f21c_d, "new-instance")                        \
  OP(NEW_ARRAY, 0x23, f22c_d, "new-array")                              \
  OP(FILLED_NEW_ARRAY, 0x24, f35c, "filled-new-array")                  \
  OP(FILLED_NEW_ARRAY_RANGE, 0x25, f3rc, "filled-new-array-range")      \
  OP(FILL_ARRAY_DATA, 0x26, f31t, "fill-array-data")                    \
  OP(THROW, 0x27, f11x_s, "throw")                                      \
  OP(GOTO, 0x28, f10t, "goto")                                          \
  OP(GOTO_16, 0x29, f20t, "goto/16")                                    \
  OP(GOTO_32, 0x2a, f30t, "goto-32")                                    \
  OP(PACKED_SWITCH, 0x2b, f31t, "packed-switch")                        \
  OP(SPARSE_SWITCH, 0x2c, f31t, "sparse-switch")                        \
  OP(CMPL_FLOAT, 0x2d, f23x_d, "cmpl-float")                            \
  OP(CMPG_FLOAT, 0x2e, f23x_d, "cmpg-float")                            \
  OP(CMPL_DOUBLE, 0x2f, f23x_d, "cmpl-double")                          \
  OP(CMPG_DOUBLE, 0x30, f23x_d, "cmpg-double")                          \
  OP(CMP_LONG, 0x31, f23x_d, "cmp-long")                                \
  OP(IF_EQ, 0x32, f22t, "if-eq")                                        \
  OP(IF_NE, 0x33, f22t, "if-ne")                                        \
  OP(IF_LT, 0x34, f22t, "if-lt")                                        \
  OP(IF_GE, 0x35, f22t, "if-ge")                                        \
  OP(IF_GT, 0x36, f22t, "if-gt")                                        \
  OP(IF_LE, 0x37, f22t, "if-le")                                        \
  OP(IF_EQZ, 0x38, f21t, "if-eqz")                                      \
  OP(IF_NEZ, 0x39, f21t, "if-nez")                                      \
  OP(IF_LTZ, 0x3a, f21t, "if-ltz")                                      \
  OP(IF_GEZ, 0x3b, f21t, "if-gez")                                      \
  OP(IF_GTZ, 0x3c, f21t, "if-gtz")                                      \
  OP(IF_LEZ, 0x3d, f21t, "if-lez")                                      \
  OP(AGET, 0x44, f23x_d, "aget")                                        \
  OP(AGET_WIDE, 0x45, f23x_d, "aget-wide")                              \
  OP(AGET_OBJECT, 0x46, f23x_d, "aget-object")                          \
  OP(AGET_BOOLEAN, 0x47, f23x_d, "aget-boolean")                        \
  OP(AGET_BYTE, 0x48, f23x_d, "aget-byte")                              \
  OP(AGET_CHAR, 0x49, f23x_d, "aget-char")                              \
  OP(AGET_SHORT, 0x4a, f23x_d, "aget-short")                            \
  OP(APUT, 0x4b, f23x_s, "aput")                                        \
  OP(APUT_WIDE, 0x4c, f23x_s, "aput-wide")                              \
  OP(APUT_OBJECT, 0x4d, f23x_s, "aput-object")                          \
  OP(APUT_BOOLEAN, 0x4e, f23x_s, "aput-boolean")                        \
  OP(APUT_BYTE, 0x4f, f23x_s, "aput-byte")                              \
  OP(APUT_CHAR, 0x50, f23x_s, "aput-char")                              \
  OP(APUT_SHORT, 0x51, f23x_s, "aput-short")                            \
  OP(IGET, 0x52, f22c_d, "iget")                                        \
  OP(IGET_WIDE, 0x53, f22c_d, "iget-wide")                              \
  OP(IGET_OBJECT, 0x54, f22c_d, "iget-object")                          \
  OP(IGET_BOOLEAN, 0x55, f22c_d, "iget-boolean")                        \
  OP(IGET_BYTE, 0x56, f22c_d, "iget-byte")                              \
  OP(IGET_CHAR, 0x57, f22c_d, "iget-char")                              \
  OP(IGET_SHORT, 0x58, f22c_d, "iget-short")                            \
  OP(IPUT, 0x59, f22c_s, "iput")                                        \
  OP(IPUT_WIDE, 0x5a, f22c_s, "iput-wide")                              \
  OP(IPUT_OBJECT, 0x5b, f22c_s, "iput-object")                          \
  OP(IPUT_BOOLEAN, 0x5c, f22c_s, "iput-boolean")                        \
  OP(IPUT_BYTE, 0x5d, f22c_s, "iput-byte")                              \
  OP(IPUT_CHAR, 0x5e, f22c_s, "iput-char")                              \
  OP(IPUT_SHORT, 0x5f, f22c_s, "iput-short")                            \
  OP(SGET, 0x60, f21c_d, "sget")                                        \
  OP(SGET_WIDE, 0x61, f21c_d, "sget-wide")                              \
  OP(SGET_OBJECT, 0x62, f21c_d, "sget-object")                          \
  OP(SGET_BOOLEAN, 0x63, f21c_d, "sget-boolean")                        \
  OP(SGET_BYTE, 0x64, f21c_d, "sget-byte")                              \
  OP(SGET_CHAR, 0x65, f21c_d, "sget-char")                              \
  OP(SGET_SHORT, 0x66, f21c_d, "sget-short")                            \
  OP(SPUT, 0x67, f21c_s, "sput")                                        \
  OP(SPUT_WIDE, 0x68, f21c_s, "sput-wide")                              \
  OP(SPUT_OBJECT, 0x69, f21c_s, "sput-object")                          \
  OP(SPUT_BOOLEAN, 0x6a, f21c_s, "sput-boolean")                        \
  OP(SPUT_BYTE, 0x6b, f21c_s, "sput-byte")                              \
  OP(SPUT_CHAR, 0x6c, f21c_s, "sput-char")                              \
  OP(SPUT_SHORT, 0x6d, f21c_s, "sput-short")                            \
  OP(INVOKE_VIRTUAL, 0x6e, f35c, "invoke-virtual")                      \
  OP(INVOKE_SUPER, 0x6f, f35c, "invoke-super")                          \
  OP(INVOKE_DIRECT, 0x70, f35c, "invoke-direct")                        \
  OP(INVOKE_STATIC, 0x71, f35c, "invoke-static")                        \
  OP(INVOKE_INTERFACE, 0x72, f35c, "invoke-interface")                  \
  OP(INVOKE_VIRTUAL_RANGE, 0x74, f3rc, "invoke-virtual-range")          \
  OP(INVOKE_SUPER_RANGE, 0x75, f3rc, "invoke-super-range")              \
  OP(INVOKE_DIRECT_RANGE, 0x76, f3rc, "invoke-direct-range")            \
  OP(INVOKE_STATIC_RANGE, 0x77, f3rc, "invoke-static-range")            \
  OP(INVOKE_INTERFACE_RANGE, 0x78, f3rc, "invoke-interface-range")      \
  OP(INVOKE_POLYMORPHIC, 0xfa, f45cc, "invoke-polymorphic")             \
  OP(INVOKE_POLYMORPHIC_RANGE, 0xfb, f4rcc, "invoke-polymorphic-range") \
  OP(INVOKE_CUSTOM, 0xfc, f35c, "invoke-custom")                        \
  OP(INVOKE_CUSTOM_RANGE, 0xfd, f3rc, "invoke-custom-range")            \
  OP(NEG_INT, 0x7b, f12x, "neg-int")                                    \
  OP(NOT_INT, 0x7c, f12x, "not-int")                                    \
  OP(NEG_LONG, 0x7d, f12x, "neg-long")                                  \
  OP(NOT_LONG, 0x7e, f12x, "not-long")                                  \
  OP(NEG_FLOAT, 0x7f, f12x, "neg-float")                                \
  OP(NEG_DOUBLE, 0x80, f12x, "neg-double")                              \
  OP(INT_TO_LONG, 0x81, f12x, "int-to-long")                            \
  OP(INT_TO_FLOAT, 0x82, f12x, "int-to-float")                          \
  OP(INT_TO_DOUBLE, 0x83, f12x, "int-to-double")                        \
  OP(LONG_TO_INT, 0x84, f12x, "long-to-int")                            \
  OP(LONG_TO_FLOAT, 0x85, f12x, "long-to-float")                        \
  OP(LONG_TO_DOUBLE, 0x86, f12x, "long-to-double")                      \
  OP(FLOAT_TO_INT, 0x87, f12x, "float-to-int")                          \
  OP(FLOAT_TO_LONG, 0x88, f12x, "float-to-long")                        \
  OP(FLOAT_TO_DOUBLE, 0x89, f12x, "float-to-double")                    \
  OP(DOUBLE_TO_INT, 0x8a, f12x, "double-to-int")                        \
  OP(DOUBLE_TO_LONG, 0x8b, f12x, "double-to-long")                      \
  OP(DOUBLE_TO_FLOAT, 0x8c, f12x, "double-to-float")                    \
  OP(INT_TO_BYTE, 0x8d, f12x, "int-to-byte")                            \
  OP(INT_TO_CHAR, 0x8e, f12x, "int-to-char")                            \
  OP(INT_TO_SHORT, 0x8f, f12x, "int-to-short")                          \
  OP(ADD_INT, 0x90, f23x_d, "add-int")                                  \
  OP(SUB_INT, 0x91, f23x_d, "sub-int")                                  \
  OP(MUL_INT, 0x92, f23x_d, "mul-int")                                  \
  OP(DIV_INT, 0x93, f23x_d, "div-int")                                  \
  OP(REM_INT, 0x94, f23x_d, "rem-int")                                  \
  OP(AND_INT, 0x95, f23x_d, "and-int")                                  \
  OP(OR_INT, 0x96, f23x_d, "or-int")                                    \
  OP(XOR_INT, 0x97, f23x_d, "xor-int")                                  \
  OP(SHL_INT, 0x98, f23x_d, "shl-int")                                  \
  OP(SHR_INT, 0x99, f23x_d, "shr-int")                                  \
  OP(USHR_INT, 0x9a, f23x_d, "ushr-int")                                \
  OP(ADD_LONG, 0x9b, f23x_d, "add-long")                                \
  OP(SUB_LONG, 0x9c, f23x_d, "sub-long")                                \
  OP(MUL_LONG, 0x9d, f23x_d, "mul-long")                                \
  OP(DIV_LONG, 0x9e, f23x_d, "div-long")                                \
  OP(REM_LONG, 0x9f, f23x_d, "rem-long")                                \
  OP(AND_LONG, 0xa0, f23x_d, "and-long")                                \
  OP(OR_LONG, 0xa1, f23x_d, "or-long")                                  \
  OP(XOR_LONG, 0xa2, f23x_d, "xor-long")                                \
  OP(SHL_LONG, 0xa3, f23x_d, "shl-long")                                \
  OP(SHR_LONG, 0xa4, f23x_d, "shr-long")                                \
  OP(USHR_LONG, 0xa5, f23x_d, "ushr-long")                              \
  OP(ADD_FLOAT, 0xa6, f23x_d, "add-float")                              \
  OP(SUB_FLOAT, 0xa7, f23x_d, "sub-float")                              \
  OP(MUL_FLOAT, 0xa8, f23x_d, "mul-float")                              \
  OP(DIV_FLOAT, 0xa9, f23x_d, "div-float")                              \
  OP(REM_FLOAT, 0xaa, f23x_d, "rem-float")                              \
  OP(ADD_DOUBLE, 0xab, f23x_d, "add-double")                            \
  OP(SUB_DOUBLE, 0xac, f23x_d, "sub-double")                            \
  OP(MUL_DOUBLE, 0xad, f23x_d, "mul-double")                            \
  OP(DIV_DOUBLE, 0xae, f23x_d, "div-double")                            \
  OP(REM_DOUBLE, 0xaf, f23x_d, "rem-double")                            \
  OP(ADD_INT_2ADDR, 0xb0, f12x_2, "add-int/2addr")                      \
  OP(SUB_INT_2ADDR, 0xb1, f12x_2, "sub-int/2addr")                      \
  OP(MUL_INT_2ADDR, 0xb2, f12x_2, "mul-int/2addr")                      \
  OP(DIV_INT_2ADDR, 0xb3, f12x_2, "div-int/2addr")                      \
  OP(REM_INT_2ADDR, 0xb4, f12x_2, "rem-int/2addr")                      \
  OP(AND_INT_2ADDR, 0xb5, f12x_2, "and-int/2addr")                      \
  OP(OR_INT_2ADDR, 0xb6, f12x_2, "or-int/2addr")                        \
  OP(XOR_INT_2ADDR, 0xb7, f12x_2, "xor-int/2addr")                      \
  OP(SHL_INT_2ADDR, 0xb8, f12x_2, "shl-int/2addr")                      \
  OP(SHR_INT_2ADDR, 0xb9, f12x_2, "shr-int/2addr")                      \
  OP(USHR_INT_2ADDR, 0xba, f12x_2, "ushr-int/2addr")                    \
  OP(ADD_LONG_2ADDR, 0xbb, f12x_2, "add-long/2addr")                    \
  OP(SUB_LONG_2ADDR, 0xbc, f12x_2, "sub-long/2addr")                    \
  OP(MUL_LONG_2ADDR, 0xbd, f12x_2, "mul-long/2addr")                    \
  OP(DIV_LONG_2ADDR, 0xbe, f12x_2, "div-long/2addr")                    \
  OP(REM_LONG_2ADDR, 0xbf, f12x_2, "rem-long/2addr")                    \
  OP(AND_LONG_2ADDR, 0xc0, f12x_2, "and-long/2addr")                    \
  OP(OR_LONG_2ADDR, 0xc1, f12x_2, "or-long/2addr")                      \
  OP(XOR_LONG_2ADDR, 0xc2, f12x_2, "xor-long/2addr")                    \
  OP(SHL_LONG_2ADDR, 0xc3, f12x_2, "shl-long/2addr")                    \
  OP(SHR_LONG_2ADDR, 0xc4, f12x_2, "shr-long/2addr")                    \
  OP(USHR_LONG_2ADDR, 0xc5, f12x_2, "ushr-long/2addr")                  \
  OP(ADD_FLOAT_2ADDR, 0xc6, f12x_2, "add-float/2addr")                  \
  OP(SUB_FLOAT_2ADDR, 0xc7, f12x_2, "sub-float/2addr")                  \
  OP(MUL_FLOAT_2ADDR, 0xc8, f12x_2, "mul-float/2addr")                  \
  OP(DIV_FLOAT_2ADDR, 0xc9, f12x_2, "div-float/2addr")                  \
  OP(REM_FLOAT_2ADDR, 0xca, f12x_2, "rem-float/2addr")                  \
  OP(ADD_DOUBLE_2ADDR, 0xcb, f12x_2, "add-double/2addr")                \
  OP(SUB_DOUBLE_2ADDR, 0xcc, f12x_2, "sub-double/2addr")                \
  OP(MUL_DOUBLE_2ADDR, 0xcd, f12x_2, "mul-double/2addr")                \
  OP(DIV_DOUBLE_2ADDR, 0xce, f12x_2, "div-double/2addr")                \
  OP(REM_DOUBLE_2ADDR, 0xcf, f12x_2, "rem-double/2addr")                \
  OP(ADD_INT_LIT16, 0xd0, f22s, "add-int/lit16")                        \
  OP(RSUB_INT, 0xd1, f22s, "rsub-int")                                  \
  OP(MUL_INT_LIT16, 0xd2, f22s, "mul-int/lit16")                        \
  OP(DIV_INT_LIT16, 0xd3, f22s, "div-int/lit16")                        \
  OP(REM_INT_LIT16, 0xd4, f22s, "rem-int/lit16")                        \
  OP(AND_INT_LIT16, 0xd5, f22s, "and-int/lit16")                        \
  OP(OR_INT_LIT16, 0xd6, f22s, "or-int/lit16")                          \
  OP(XOR_INT_LIT16, 0xd7, f22s, "xor-int/lit16")                        \
  OP(ADD_INT_LIT8, 0xd8, f22b, "add-int/lit8")                          \
  OP(RSUB_INT_LIT8, 0xd9, f22b, "rsub-int/lit8")                        \
  OP(MUL_INT_LIT8, 0xda, f22b, "mul-int/lit8")                          \
  OP(DIV_INT_LIT8, 0xdb, f22b, "div-int/lit8")                          \
  OP(REM_INT_LIT8, 0xdc, f22b, "rem-int/lit8")                          \
  OP(AND_INT_LIT8, 0xdd, f22b, "and-int/lit8")                          \
  OP(OR_INT_LIT8, 0xde, f22b, "or-int/lit8")                            \
  OP(XOR_INT_LIT8, 0xdf, f22b, "xor-int/lit8")                          \
  OP(SHL_INT_LIT8, 0xe0, f22b, "shl-int/lit8")                          \
  OP(SHR_INT_LIT8, 0xe1, f22b, "shr-int/lit8")                          \
  OP(USHR_INT_LIT8, 0xe2, f22b, "ushr-int/lit8")

#define QDOPS                                                              \
  OP(RETURN_VOID_NO_BARRIER, 0x73, f10x, "return-void-no-barrier")         \
  OP(IGET_QUICK, 0xe3, f22c_d, "iget-quick")                               \
  OP(IGET_WIDE_QUICK, 0xe4, f22c_d, "iget-wide-quick")                     \
  OP(IGET_OBJECT_QUICK, 0xe5, f22c_d, "iget-object-quick")                 \
  OP(IPUT_QUICK, 0xe6, f22c_s, "iput-quick")                               \
  OP(IPUT_WIDE_QUICK, 0xe7, f22c_s, "iput-wide-quick")                     \
  OP(IPUT_OBJECT_QUICK, 0xe8, f22c_s, "iput-object-quick")                 \
  OP(INVOKE_VIRTUAL_QUICK, 0xe9, f35c, "invoke-virtual-quick")             \
  OP(INVOKE_VIRTUAL_RANGE_QUICK, 0xea, f3rc, "invoke-virtual/range-quick") \
  OP(IPUT_BOOLEAN_QUICK, 0xeb, f22c_s, "iput-boolean-quick")               \
  OP(IPUT_BYTE_QUICK, 0xec, f22c_s, "iput-byte-quick")                     \
  OP(IPUT_CHAR_QUICK, 0xed, f22c_s, "iput-char-quick")                     \
  OP(IPUT_SHORT_QUICK, 0xee, f22c_s, "iput-short-quick")                   \
  OP(IGET_BOOLEAN_QUICK, 0xef, f22c_d, "iget-boolean-quick")               \
  OP(IGET_BYTE_QUICK, 0xf0, f22c_d, "iget-byte-quick")                     \
  OP(IGET_CHAR_QUICK, 0xf1, f22c_d, "iget-char-quick")                     \
  OP(IGET_SHORT_QUICK, 0xf2, f22c_d, "iget-short-quick")

// clang-format off
enum DexOpcode : uint16_t {
#define OP(op, code, ...) DOPCODE_##op = code,
  DOPS
  QDOPS
#undef OP
  FOPCODE_PACKED_SWITCH = 0x0100,
  FOPCODE_SPARSE_SWITCH = 0x0200,
  FOPCODE_FILLED_ARRAY = 0x0300,
};
// clang-format on

#define SWITCH_FORMAT_10           \
  case DOPCODE_MOVE:               \
  case DOPCODE_MOVE_WIDE:          \
  case DOPCODE_MOVE_OBJECT:        \
  case DOPCODE_MOVE_RESULT:        \
  case DOPCODE_MOVE_RESULT_WIDE:   \
  case DOPCODE_MOVE_RESULT_OBJECT: \
  case DOPCODE_MOVE_EXCEPTION:     \
  case DOPCODE_RETURN_VOID:        \
  case DOPCODE_RETURN:             \
  case DOPCODE_RETURN_WIDE:        \
  case DOPCODE_RETURN_OBJECT:      \
  case DOPCODE_CONST_4:            \
  case DOPCODE_MONITOR_ENTER:      \
  case DOPCODE_MONITOR_EXIT:       \
  case DOPCODE_THROW:              \
  case DOPCODE_GOTO:               \
  case DOPCODE_NEG_INT:            \
  case DOPCODE_NOT_INT:            \
  case DOPCODE_NEG_LONG:           \
  case DOPCODE_NOT_LONG:           \
  case DOPCODE_NEG_FLOAT:          \
  case DOPCODE_NEG_DOUBLE:         \
  case DOPCODE_INT_TO_LONG:        \
  case DOPCODE_INT_TO_FLOAT:       \
  case DOPCODE_INT_TO_DOUBLE:      \
  case DOPCODE_LONG_TO_INT:        \
  case DOPCODE_LONG_TO_FLOAT:      \
  case DOPCODE_LONG_TO_DOUBLE:     \
  case DOPCODE_FLOAT_TO_INT:       \
  case DOPCODE_FLOAT_TO_LONG:      \
  case DOPCODE_FLOAT_TO_DOUBLE:    \
  case DOPCODE_DOUBLE_TO_INT:      \
  case DOPCODE_DOUBLE_TO_LONG:     \
  case DOPCODE_DOUBLE_TO_FLOAT:    \
  case DOPCODE_INT_TO_BYTE:        \
  case DOPCODE_INT_TO_CHAR:        \
  case DOPCODE_INT_TO_SHORT:       \
  case DOPCODE_ADD_INT_2ADDR:      \
  case DOPCODE_SUB_INT_2ADDR:      \
  case DOPCODE_MUL_INT_2ADDR:      \
  case DOPCODE_DIV_INT_2ADDR:      \
  case DOPCODE_REM_INT_2ADDR:      \
  case DOPCODE_AND_INT_2ADDR:      \
  case DOPCODE_OR_INT_2ADDR:       \
  case DOPCODE_XOR_INT_2ADDR:      \
  case DOPCODE_SHL_INT_2ADDR:      \
  case DOPCODE_SHR_INT_2ADDR:      \
  case DOPCODE_USHR_INT_2ADDR:     \
  case DOPCODE_ADD_LONG_2ADDR:     \
  case DOPCODE_SUB_LONG_2ADDR:     \
  case DOPCODE_MUL_LONG_2ADDR:     \
  case DOPCODE_DIV_LONG_2ADDR:     \
  case DOPCODE_REM_LONG_2ADDR:     \
  case DOPCODE_AND_LONG_2ADDR:     \
  case DOPCODE_OR_LONG_2ADDR:      \
  case DOPCODE_XOR_LONG_2ADDR:     \
  case DOPCODE_SHL_LONG_2ADDR:     \
  case DOPCODE_SHR_LONG_2ADDR:     \
  case DOPCODE_USHR_LONG_2ADDR:    \
  case DOPCODE_ADD_FLOAT_2ADDR:    \
  case DOPCODE_SUB_FLOAT_2ADDR:    \
  case DOPCODE_MUL_FLOAT_2ADDR:    \
  case DOPCODE_DIV_FLOAT_2ADDR:    \
  case DOPCODE_REM_FLOAT_2ADDR:    \
  case DOPCODE_ADD_DOUBLE_2ADDR:   \
  case DOPCODE_SUB_DOUBLE_2ADDR:   \
  case DOPCODE_MUL_DOUBLE_2ADDR:   \
  case DOPCODE_DIV_DOUBLE_2ADDR:   \
  case DOPCODE_REM_DOUBLE_2ADDR:   \
  case DOPCODE_ARRAY_LENGTH:

#define SWITCH_FORMAT_RETURN_VOID_NO_BARRIER \
  case DOPCODE_RETURN_VOID_NO_BARRIER:

#define SWITCH_FORMAT_20           \
  case DOPCODE_MOVE_FROM16:        \
  case DOPCODE_MOVE_WIDE_FROM16:   \
  case DOPCODE_MOVE_OBJECT_FROM16: \
  case DOPCODE_CONST_16:           \
  case DOPCODE_CONST_HIGH16:       \
  case DOPCODE_CONST_WIDE_16:      \
  case DOPCODE_CONST_WIDE_HIGH16:  \
  case DOPCODE_GOTO_16:            \
  case DOPCODE_CMPL_FLOAT:         \
  case DOPCODE_CMPG_FLOAT:         \
  case DOPCODE_CMPL_DOUBLE:        \
  case DOPCODE_CMPG_DOUBLE:        \
  case DOPCODE_CMP_LONG:           \
  case DOPCODE_IF_EQ:              \
  case DOPCODE_IF_NE:              \
  case DOPCODE_IF_LT:              \
  case DOPCODE_IF_GE:              \
  case DOPCODE_IF_GT:              \
  case DOPCODE_IF_LE:              \
  case DOPCODE_IF_EQZ:             \
  case DOPCODE_IF_NEZ:             \
  case DOPCODE_IF_LTZ:             \
  case DOPCODE_IF_GEZ:             \
  case DOPCODE_IF_GTZ:             \
  case DOPCODE_IF_LEZ:             \
  case DOPCODE_AGET:               \
  case DOPCODE_AGET_WIDE:          \
  case DOPCODE_AGET_OBJECT:        \
  case DOPCODE_AGET_BOOLEAN:       \
  case DOPCODE_AGET_BYTE:          \
  case DOPCODE_AGET_CHAR:          \
  case DOPCODE_AGET_SHORT:         \
  case DOPCODE_APUT:               \
  case DOPCODE_APUT_WIDE:          \
  case DOPCODE_APUT_OBJECT:        \
  case DOPCODE_APUT_BOOLEAN:       \
  case DOPCODE_APUT_BYTE:          \
  case DOPCODE_APUT_CHAR:          \
  case DOPCODE_APUT_SHORT:         \
  case DOPCODE_ADD_INT:            \
  case DOPCODE_SUB_INT:            \
  case DOPCODE_MUL_INT:            \
  case DOPCODE_DIV_INT:            \
  case DOPCODE_REM_INT:            \
  case DOPCODE_AND_INT:            \
  case DOPCODE_OR_INT:             \
  case DOPCODE_XOR_INT:            \
  case DOPCODE_SHL_INT:            \
  case DOPCODE_SHR_INT:            \
  case DOPCODE_USHR_INT:           \
  case DOPCODE_ADD_LONG:           \
  case DOPCODE_SUB_LONG:           \
  case DOPCODE_MUL_LONG:           \
  case DOPCODE_DIV_LONG:           \
  case DOPCODE_REM_LONG:           \
  case DOPCODE_AND_LONG:           \
  case DOPCODE_OR_LONG:            \
  case DOPCODE_XOR_LONG:           \
  case DOPCODE_SHL_LONG:           \
  case DOPCODE_SHR_LONG:           \
  case DOPCODE_USHR_LONG:          \
  case DOPCODE_ADD_FLOAT:          \
  case DOPCODE_SUB_FLOAT:          \
  case DOPCODE_MUL_FLOAT:          \
  case DOPCODE_DIV_FLOAT:          \
  case DOPCODE_REM_FLOAT:          \
  case DOPCODE_ADD_DOUBLE:         \
  case DOPCODE_SUB_DOUBLE:         \
  case DOPCODE_MUL_DOUBLE:         \
  case DOPCODE_DIV_DOUBLE:         \
  case DOPCODE_REM_DOUBLE:         \
  case DOPCODE_ADD_INT_LIT16:      \
  case DOPCODE_RSUB_INT:           \
  case DOPCODE_MUL_INT_LIT16:      \
  case DOPCODE_DIV_INT_LIT16:      \
  case DOPCODE_REM_INT_LIT16:      \
  case DOPCODE_AND_INT_LIT16:      \
  case DOPCODE_OR_INT_LIT16:       \
  case DOPCODE_XOR_INT_LIT16:      \
  case DOPCODE_ADD_INT_LIT8:       \
  case DOPCODE_RSUB_INT_LIT8:      \
  case DOPCODE_MUL_INT_LIT8:       \
  case DOPCODE_DIV_INT_LIT8:       \
  case DOPCODE_REM_INT_LIT8:       \
  case DOPCODE_AND_INT_LIT8:       \
  case DOPCODE_OR_INT_LIT8:        \
  case DOPCODE_XOR_INT_LIT8:       \
  case DOPCODE_SHL_INT_LIT8:       \
  case DOPCODE_SHR_INT_LIT8:       \
  case DOPCODE_USHR_INT_LIT8:

#define SWITCH_FORMAT_30        \
  case DOPCODE_MOVE_16:         \
  case DOPCODE_MOVE_WIDE_16:    \
  case DOPCODE_MOVE_OBJECT_16:  \
  case DOPCODE_CONST:           \
  case DOPCODE_CONST_WIDE_32:   \
  case DOPCODE_FILL_ARRAY_DATA: \
  case DOPCODE_GOTO_32:         \
  case DOPCODE_PACKED_SWITCH:   \
  case DOPCODE_SPARSE_SWITCH:

#define SWITCH_FORMAT_50 case DOPCODE_CONST_WIDE:

#define SWITCH_FORMAT_REGULAR_FIELD_REF \
  case DOPCODE_IGET:                    \
  case DOPCODE_IGET_WIDE:               \
  case DOPCODE_IGET_OBJECT:             \
  case DOPCODE_IGET_BOOLEAN:            \
  case DOPCODE_IGET_BYTE:               \
  case DOPCODE_IGET_CHAR:               \
  case DOPCODE_IGET_SHORT:              \
  case DOPCODE_IPUT:                    \
  case DOPCODE_IPUT_WIDE:               \
  case DOPCODE_IPUT_OBJECT:             \
  case DOPCODE_IPUT_BOOLEAN:            \
  case DOPCODE_IPUT_BYTE:               \
  case DOPCODE_IPUT_CHAR:               \
  case DOPCODE_IPUT_SHORT:              \
  case DOPCODE_SGET:                    \
  case DOPCODE_SGET_WIDE:               \
  case DOPCODE_SGET_OBJECT:             \
  case DOPCODE_SGET_BOOLEAN:            \
  case DOPCODE_SGET_BYTE:               \
  case DOPCODE_SGET_CHAR:               \
  case DOPCODE_SGET_SHORT:              \
  case DOPCODE_SPUT:                    \
  case DOPCODE_SPUT_WIDE:               \
  case DOPCODE_SPUT_OBJECT:             \
  case DOPCODE_SPUT_BOOLEAN:            \
  case DOPCODE_SPUT_BYTE:               \
  case DOPCODE_SPUT_CHAR:               \
  case DOPCODE_SPUT_SHORT:

#define SWITCH_FORMAT_QUICK_FIELD_REF \
  case DOPCODE_IGET_QUICK:            \
  case DOPCODE_IGET_WIDE_QUICK:       \
  case DOPCODE_IGET_OBJECT_QUICK:     \
  case DOPCODE_IPUT_QUICK:            \
  case DOPCODE_IPUT_WIDE_QUICK:       \
  case DOPCODE_IPUT_OBJECT_QUICK:     \
  case DOPCODE_IPUT_BOOLEAN_QUICK:    \
  case DOPCODE_IPUT_BYTE_QUICK:       \
  case DOPCODE_IPUT_CHAR_QUICK:       \
  case DOPCODE_IPUT_SHORT_QUICK:      \
  case DOPCODE_IGET_BOOLEAN_QUICK:    \
  case DOPCODE_IGET_BYTE_QUICK:       \
  case DOPCODE_IGET_CHAR_QUICK:       \
  case DOPCODE_IGET_SHORT_QUICK:

#define SWITCH_FORMAT_REGULAR_METHOD_REF \
  case DOPCODE_INVOKE_VIRTUAL:           \
  case DOPCODE_INVOKE_SUPER:             \
  case DOPCODE_INVOKE_DIRECT:            \
  case DOPCODE_INVOKE_STATIC:            \
  case DOPCODE_INVOKE_INTERFACE:         \
  case DOPCODE_INVOKE_CUSTOM:            \
  case DOPCODE_INVOKE_POLYMORPHIC:       \
  case DOPCODE_INVOKE_VIRTUAL_RANGE:     \
  case DOPCODE_INVOKE_SUPER_RANGE:       \
  case DOPCODE_INVOKE_DIRECT_RANGE:      \
  case DOPCODE_INVOKE_STATIC_RANGE:      \
  case DOPCODE_INVOKE_INTERFACE_RANGE:   \
  case DOPCODE_INVOKE_CUSTOM_RANGE:      \
  case DOPCODE_INVOKE_POLYMORPHIC_RANGE:

#define SWITCH_FORMAT_QUICK_METHOD_REF \
  case DOPCODE_INVOKE_VIRTUAL_QUICK:   \
  case DOPCODE_INVOKE_VIRTUAL_RANGE_QUICK:

#define SWITCH_FORMAT_CONST_STRING case DOPCODE_CONST_STRING:

#define SWITCH_FORMAT_CONST_STRING_JUMBO case DOPCODE_CONST_STRING_JUMBO:

#define SWITCH_FORMAT_TYPE_REF \
  case DOPCODE_CONST_CLASS:    \
  case DOPCODE_CHECK_CAST:     \
  case DOPCODE_INSTANCE_OF:    \
  case DOPCODE_NEW_INSTANCE:   \
  case DOPCODE_NEW_ARRAY:

#define SWITCH_FORMAT_FILL_ARRAY \
  case DOPCODE_FILLED_NEW_ARRAY: \
  case DOPCODE_FILLED_NEW_ARRAY_RANGE:

std::string print(DexOpcode opcode);

DexOpcode quicken(DexOpcode opcode);
