/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "Creators.h"
#include "IRCode.h"
#include "S_Expression.h"

/*
 * This module provides an easy way to create / serialize Dex elements using
 * S-expressions.
 *
 * Example syntax:
 *
 * (method (public static) "LFoo;.bar()V"
 *  (
 *   (const v0 0)
 *   (:L0)
 *   (sget-object "LFoo.bar:I")
 *   (move-result-pseudo-object v1)
 *   ; note that since invoke-* instructions can take a variable number of
 *   ; src operands, we wrap them in a list.
 *   (invoke-static (v0 v1) "LFoo.qux:(II)V")
 *   (goto :L0)
 *  )
 * )
 *
 * Note that any field or methods that the opcodes reference will be
 * automatically created by the assembler. I.e. you do *not* need to call
 * make_{field,method}() beforehand to ensure that they exist.
 *
 * Not-yet-implemented features:
 *   - try-catch
 *   - fill-array-data opcodes
 *
 * NOTE:
 * When assembling an IRCode instance, the assembler will attempt to set the
 * registers_size for you by making it 1 larger than the largest register
 * operand in the instruction list. Note that this is *not* always correct if
 * the registers are being interpreted as virtual registers instead of symbolic
 * ones. In that case, if the largest register operand is a wide operand, the
 * registers_size should be set to that register + 2. If you need to treat
 * registers as non-symbolic, you'll need to calculate and set the correct
 * registers_size yourself. We should move all our optimizations to treat
 * registers symbolically anyway, so this minor inconvenience should eventually
 * be a non-issue.
 */

namespace assembler {

sparta::s_expr to_s_expr(const IRCode* code);

inline std::string to_string(const IRCode* code) {
  return to_s_expr(code).str();
}

std::unique_ptr<IRCode> ircode_from_s_expr(const sparta::s_expr&);

std::unique_ptr<IRCode> ircode_from_string(const std::string&);

DexMethod* method_from_s_expr(const sparta::s_expr&);

DexMethod* method_from_string(const std::string&);

DexMethod* class_with_method(const std::string& class_name,
                             const std::string& method_instructions);

DexClass* class_with_methods(const std::string& class_name,
                             const std::vector<DexMethod*>& methods);

} // namespace assembler
