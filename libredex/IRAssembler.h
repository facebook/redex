/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "IRCode.h"
#include "S_Expression.h"

/*
 * This module provides an easy way to create / serialize sequences of
 * IRInstructions using S-expressions.
 *
 * Example:
 *
 *   (const v0 0)
 *   :L0
 *   (sget-object "LFoo.bar:I")
 *   (move-result-pseudo-object v1)
 *   ; note that since invoke-* instructions can take a variable number of
 *   ; src operands, we wrap them in a list.
 *   (invoke-static (v0 v1) "LFoo.qux:(II)V")
 *   (goto :L0)
 *
 * Note that any field or methods that the opcodes reference will be
 * automatically created by the assembler. I.e. you do *not* need to call
 * make_{field,method}() beforehand to ensure that they exist.
 *
 * Not-yet-implemented features:
 *   - try-catch
 *   - debug info
 *   - fill-array-data opcodes
 */

namespace assembler {

s_expr to_s_expr(const IRCode* code);

inline std::string to_string(const IRCode* code) {
  return to_s_expr(code).str();
}

std::unique_ptr<IRCode> ircode_from_s_expr(const s_expr&);

std::unique_ptr<IRCode> ircode_from_string(const std::string&);

} // assembler
