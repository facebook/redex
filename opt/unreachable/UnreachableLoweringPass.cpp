/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UnreachableLoweringPass.h"

#include "ControlFlow.h"
#include "Lazy.h"
#include "LiveRange.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {

constexpr const char* METRIC_UNREACHABLE_INSTRUCTIONS =
    "unreachable_instructions";
constexpr const char* METRIC_UNREACHABLE_METHODS = "unreachable_methods";
constexpr const char* METRIC_REACHABLE_METHODS_WITH_UNREACHABLE_INSTRUCTIONS =
    "reachable_methods_with_unreachable_instructions";

} // namespace

void UnreachableLoweringPass::run_pass(DexStoresVector& stores,
                                       ConfigFiles& conf,
                                       PassManager& mgr) {
  const auto scope = build_class_scope(stores);
  std::atomic<size_t> unreachable_instructions{0};
  std::atomic<size_t> unreachable_methods{0};
  std::atomic<size_t> reachable_methods_with_unreachable_instructions{0};
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    auto& cfg = code.cfg();
    bool is_unreachable_method = cfg.entry_block()->is_unreachable();
    if (is_unreachable_method) {
      unreachable_methods++;
    }
    size_t local_unreachable_instructions{0};
    Lazy<live_range::DefUseChains> duchains([&cfg]() {
      live_range::Chains chains(cfg);
      return chains.get_def_use_chains();
    });
    for (auto& mie : InstructionIterable(cfg)) {
      if (!opcode::is_unreachable(mie.insn->opcode())) {
        continue;
      }
      local_unreachable_instructions++;
      auto& uses = (*duchains)[mie.insn];
      for (auto& use : uses) {
        auto throw_insn = uses.begin()->insn;
        always_assert_log(opcode::is_throw(throw_insn->opcode()),
                          "only unreachable instruction {%s} use {%s} must be "
                          "throw in %s:\n%s",
                          SHOW(mie.insn), SHOW(throw_insn), SHOW(method),
                          SHOW(cfg));
      }

      // TODO: Consider other transformations, e.g. just return if there are no
      // monitor instructions, or embed a descriptive message.
      mie.insn->set_opcode(OPCODE_CONST);
      mie.insn->set_literal(0);
    }
    if (local_unreachable_instructions > 0) {
      unreachable_instructions += local_unreachable_instructions;
      if (!is_unreachable_method) {
        reachable_methods_with_unreachable_instructions++;
      }
    }
  });
  mgr.incr_metric(METRIC_UNREACHABLE_INSTRUCTIONS,
                  (size_t)unreachable_instructions);
  mgr.incr_metric(METRIC_UNREACHABLE_METHODS, (size_t)unreachable_methods);
  mgr.incr_metric(METRIC_REACHABLE_METHODS_WITH_UNREACHABLE_INSTRUCTIONS,
                  (size_t)reachable_methods_with_unreachable_instructions);
  TRACE(UNREACHABLE, 1,
        "%zu unreachable instructions, %zu unreachable methods, %zu reachable "
        "methods with unreachable instructions",
        (size_t)unreachable_instructions, (size_t)unreachable_methods,
        (size_t)reachable_methods_with_unreachable_instructions);
}

static UnreachableLoweringPass s_pass;
