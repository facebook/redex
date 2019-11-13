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

#include <boost/optional.hpp>

namespace {

/// \return a new \c IRInstruction representing a `const` operation writing
/// literal \p lit into register \p dest.
IRInstruction* ir_const(uint32_t dest, int64_t lit) {
  auto insn = new IRInstruction(OPCODE_CONST);
  insn->set_dest(dest);
  insn->set_literal(lit);
  return insn;
}

/// \return a new \c IRInstruction representing a `throw` operation, throwing
/// the contents of register \p src.
IRInstruction* ir_throw(uint32_t src) {
  auto insn = new IRInstruction(OPCODE_THROW);
  insn->set_src(0, src);
  return insn;
}

} // namespace

void RemoveUninstantiablesPass::remove_from_cfg(cfg::ControlFlowGraph& cfg) {
  using Insert = cfg::CFGMutation::Insert;
  cfg::CFGMutation m(cfg);

  // Lazily generate a scratch register.
  auto get_scratch = [&cfg, reg = boost::optional<uint32_t>()]() mutable {
    if (!reg) {
      reg = cfg.allocate_temp();
    }
    return *reg;
  };

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
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_VIRTUAL:
      if (is_uninstantiable_class(insn->get_method()->get_class())) {
        auto tmp = get_scratch();
        m.add_change(Insert::Replacing, it, {ir_const(tmp, 0), ir_throw(tmp)});
      }
      break;

    case OPCODE_IGET:
    case OPCODE_IGET_BYTE:
    case OPCODE_IGET_CHAR:
    case OPCODE_IGET_WIDE:
    case OPCODE_IGET_SHORT:
    case OPCODE_IGET_OBJECT:
    case OPCODE_IGET_BOOLEAN:
    case OPCODE_IPUT:
    case OPCODE_IPUT_BYTE:
    case OPCODE_IPUT_CHAR:
    case OPCODE_IPUT_WIDE:
    case OPCODE_IPUT_SHORT:
    case OPCODE_IPUT_OBJECT:
    case OPCODE_IPUT_BOOLEAN:
      if (is_uninstantiable_class(insn->get_field()->get_class())) {
        auto tmp = get_scratch();
        m.add_change(Insert::Replacing, it, {ir_const(tmp, 0), ir_throw(tmp)});
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
