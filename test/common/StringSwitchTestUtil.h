/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <cstddef>

#include "DexClass.h"
#include "IRInstruction.h"
#include "IROpcode.h"
#include "MethodUtil.h"

// Shared opcode-counting helpers for the String-switch -> StringTreeMap rewrite
// tests. The unit test counts over a built CFG and the instrumentation verifier
// counts over a ballooned method's linear code; both need the same tallies, so
// the counting loop lives here and is parameterized over the instruction
// iterable.
namespace string_switch_test {

// Opcode tallies relevant to verifying the rewrite. `lookup` counts
// invoke-static calls to the configured trie lookup method (the post-transform
// dispatch); `hashcode`/`equals`/`switches` track the original dispatch
// machinery a successful rewrite removes.
struct OpcodeCounts {
  size_t hashcode{0};
  size_t equals{0};
  size_t switches{0};
  size_t invoke_static{0};
  size_t const_string{0};
  size_t lookup{0};
};

// Tallies the rewrite-relevant opcodes over `insns` -- any IR instruction
// iterable, e.g. cfg::InstructionIterable(cfg) for a built CFG or
// InstructionIterable(code) for a ballooned method's linear code. invoke-static
// calls to `lookup_ref` (when non-null) are additionally counted in `lookup`.
template <typename InstructionIterableT>
OpcodeCounts count_string_switch_opcodes(
    InstructionIterableT&& insns, const DexMethodRef* lookup_ref = nullptr) {
  OpcodeCounts c;
  for (auto& mie : insns) {
    auto* insn = mie.insn;
    auto op = insn->opcode();
    if (opcode::is_switch(op)) {
      c.switches++;
    } else if (op == OPCODE_INVOKE_STATIC) {
      c.invoke_static++;
      if (lookup_ref != nullptr && insn->get_method() == lookup_ref) {
        c.lookup++;
      }
    } else if (op == OPCODE_CONST_STRING) {
      c.const_string++;
    } else if (op == OPCODE_INVOKE_VIRTUAL) {
      auto* m = insn->get_method();
      if (m == method::java_lang_String_hashCode()) {
        c.hashcode++;
      } else if (m == method::java_lang_String_equals()) {
        c.equals++;
      }
    }
  }
  return c;
}

} // namespace string_switch_test
