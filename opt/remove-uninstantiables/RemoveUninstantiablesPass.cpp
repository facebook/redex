/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RemoveUninstantiablesPass.h"

#include "CFGMutation.h"
#include "ControlFlow.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "Walkers.h"

namespace {

/// \return a new \c IRInstruction representing a `const` operation writing
/// literal \p lit into register \p dest.
IRInstruction* ir_const(uint32_t dest, int64_t lit) {
  auto insn = new IRInstruction(OPCODE_CONST);
  insn->set_dest(dest);
  insn->set_literal(lit);
  return insn;
}

} // namespace

void RemoveUninstantiablesPass::remove_from_cfg(cfg::ControlFlowGraph& cfg) {
  using Insert = cfg::CFGMutation::Insert;
  cfg::CFGMutation m(cfg);

  auto ii = InstructionIterable(cfg);
  for (auto it = ii.begin(); it != ii.end(); ++it) {
    auto insn = it->insn;
    switch (insn->opcode()) {
    case OPCODE_INSTANCE_OF:
      if (is_uninstantiable_class(insn->get_type())) {
        auto dest = cfg.move_result_of(it)->insn->dest();
        m.add_change(Insert::Replacing, it, {ir_const(dest, 0)});
      }
      break;
    default:
      continue;
    }
  }
}

void RemoveUninstantiablesPass::run_pass(DexStoresVector& stores,
                                         ConfigFiles&,
                                         PassManager&) {
  Scope scope = build_class_scope(stores);
  walk::parallel::methods(scope, [](DexMethod* method) {
    auto code = method->get_code();
    if (method->rstate.no_optimizations() || code == nullptr) {
      return;
    }

    code->build_cfg();
    remove_from_cfg(code->cfg());
    code->clear_cfg();
  });
}
