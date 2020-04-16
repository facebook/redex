/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "BaseIRAnalyzer.h"
#include "ConstantAbstractDomain.h"
#include "DexClass.h"

namespace monitor_count {

/*
 * The Android verifier has a check[1] to make sure that when a monitor is being
 * held, any potentially throwing opcode is wrapped in a try region that goes to
 * a catch-all block. The catch-all is ostensibly responsible for executing the
 * monitor-exit.
 *
 * Surprisingly, the verifier only performs the catch-all check for throwing
 * opcodes contained in *some* try region. This means that a method with
 * non-wrapped throwing opcodes in a synchronized block will verify. However,
 * if such a method is inlined, and its callsite was wrapped in a try region
 * that does not have a catch-all, then we have a VerifyError! More generally,
 * any code-relocating optimizations could trigger this issue. Therefore, to be
 * safe, we mark all methods with these non-wrapped throwing opcodes in monitor
 * regions as don't-inline as well as no-optimize.
 *
 * [1]:
 * http://androidxref.com/9.0.0_r3/xref/art/runtime/verifier/method_verifier.cc#3553
 */
void mark_sketchy_methods_with_no_optimize(const Scope&);

IRInstruction* find_synchronized_throw_outside_catch_all(const IRCode&);

using MonitorCountDomain = sparta::ConstantAbstractDomain<uint32_t>;

class Analyzer : public ir_analyzer::BaseIRAnalyzer<MonitorCountDomain> {
 public:
  using ir_analyzer::BaseIRAnalyzer<MonitorCountDomain>::BaseIRAnalyzer;

  void analyze_instruction(const IRInstruction* insn,
                           MonitorCountDomain* current) const override;
};

} // namespace monitor_count
