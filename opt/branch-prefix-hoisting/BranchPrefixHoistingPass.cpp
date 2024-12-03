/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "BranchPrefixHoistingPass.h"

#include "BranchPrefixHoisting.h"
#include "PassManager.h"
#include "Show.h"
#include "Walkers.h"

/**
 * This pass eliminates sibling branches that begin with identical instructions,
 * (aka prefix hoisting).
 * example code pattern
 * if (condition) {
 *   insn_1;
 *   insn_2;
 *   insn_3;
 * } else {
 *   insn_1;
 *   insn_2;
 *   insn_4;
 * }
 * will be optimized into
 * insn_1;
 * insn_2;
 * if (condition) {
 *   insn_3;
 * } else {
 *   insn_4;
 * }
 * given that the hoisted instructions doesn't have a side effect on the branch
 * condition.
 *
 * We leave debug and position info in the original block. This is required for
 * correctness of the suffix.
 *
 * We hoist source blocks. The reasoning for that is tracking of exceptional
 * flow.
 *
 * Note: if an instruction gets hoisted may throw, the line numbers in stack
 * trace may be pointing to before the branch.
 */

namespace {
constexpr const char* METRIC_INSTRUCTIONS_HOISTED = "num_instructions_hoisted";
} // namespace

void BranchPrefixHoistingPass::run_pass(DexStoresVector& stores,
                                        ConfigFiles& /* unused */,
                                        PassManager& mgr) {
  auto scope = build_class_scope(stores);

  bool can_allocate_regs = !mgr.regalloc_has_run();
  int total_insns_hoisted = walk::parallel::methods<int>(
      scope, [can_allocate_regs](DexMethod* method) -> int {
        const auto code = method->get_code();
        if (!code || method->rstate.no_optimizations()) {
          return 0;
        }
        TraceContext context{method};

        int insns_hoisted = branch_prefix_hoisting_impl::process_code(
            code, method, can_allocate_regs);
        if (insns_hoisted) {
          TRACE(BPH, 3,
                "[branch prefix hoisting] Moved %u insns in method {%s}",
                insns_hoisted, SHOW(method));
        }
        return insns_hoisted;
      });

  mgr.incr_metric(METRIC_INSTRUCTIONS_HOISTED, total_insns_hoisted);
}

static BranchPrefixHoistingPass s_pass;
