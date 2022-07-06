/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_set>

class DexField;
class DexMethod;
class IRInstruction;

namespace constructor_analysis {

// Checks that...
// - there are no assignments to (non-inherited) instance fields before
//   a constructor call, and
// - the constructor refers to a method of the same class, and
// - there are no assignments to any final fields.
// Under these conditions, a constructor is universally inlinable.
bool can_inline_init(
    const DexMethod* init_method,
    const std::unordered_set<const DexField*>* finalizable_fields = nullptr);

// Checks that the invocation of one constructor within another constructor
// of the same class can be inlined, either for a particular or for all
// callsites (when given callsite is nullptr).
// In particular, this checks that the relevant callsites invocations are
// with the this object as the receiver.
bool can_inline_inits_in_same_class(DexMethod* caller_method,
                                    const DexMethod* callee_method,
                                    IRInstruction* callsite_insn);

} // namespace constructor_analysis
