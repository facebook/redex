/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
 * any code-relocating optimizations could trigger this issue.
 *
 * [1]:
 * http://androidxref.com/9.0.0_r3/xref/art/runtime/verifier/method_verifier.cc#3553
 */

using MonitorCountDomain = sparta::ConstantAbstractDomain<uint32_t>;

class Analyzer : public ir_analyzer::BaseIRAnalyzer<MonitorCountDomain> {
 private:
  const cfg::ControlFlowGraph& m_cfg;

 public:
  using ir_analyzer::BaseIRAnalyzer<MonitorCountDomain>::BaseIRAnalyzer;
  explicit Analyzer(const cfg::ControlFlowGraph& cfg)
      : BaseIRAnalyzer(cfg), m_cfg(cfg) {
    run(MonitorCountDomain(0));
  }

  MonitorCountDomain analyze_edge(
      const EdgeId& edge,
      const MonitorCountDomain& exit_state_at_source) const override;
  void analyze_instruction(const IRInstruction* insn,
                           MonitorCountDomain* current) const override;

  // All blocks that can be reached with a different number of executed
  // monitor-enter instructions.
  std::vector<cfg::Block*> get_monitor_mismatches();

  // All instructions that can throw in synchronized blocks without
  // catch-alls. (This would be unverifiable if the instructions are in blocks
  // with other (non-catch-all) throw-edges.)
  std::vector<cfg::InstructionIterator> get_sketchy_instructions();
};

inline IRInstruction* find_synchronized_throw_outside_catch_all(
    const IRCode& code) {
  auto sketchy_insns = Analyzer(code.cfg()).get_sketchy_instructions();
  return sketchy_insns.empty() ? nullptr : sketchy_insns.front()->insn;
}

/**
 * Return true if inlining would create a synchronized block with throw-edges
 * but without a catch-all, or break monitor depth consistency. To avoid that,
 * we...
 * - reject a sketchy call-site in caller if the callee has try regions without
 *   catch-alls, and we
 * - reject a call-site that is in a try region if callee is sketchy.
 *
 * A "sketchy" instruction is an instruction that can throw in a synchronized
 * block without a catch-all.
 *
 * We are conservative if the caller or callee have try regions but no cfg.
 * The invoke_insn is optional; if not provided, the analysis is
 * conservative over all instructions.
 */
bool cannot_inline_sketchy_code(const IRCode& caller,
                                const IRCode& callee,
                                const IRInstruction* invoke_insn);

} // namespace monitor_count
