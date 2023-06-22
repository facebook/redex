/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "NoSpuriousGetClassCallsChecker.h"

#include "Debug.h"
#include "DexClass.h"
#include "ScopedCFG.h"
#include "Show.h"
#include "Walkers.h"

namespace redex_properties {

void NoSpuriousGetClassCallsChecker::check_spurious_getClass(
    DexMethod* method) {
  IRCode* code = method->get_code();
  if (code == nullptr) {
    return;
  }
  cfg::ScopedCFG cfg{code};
  for (auto* block : cfg->blocks()) {
    for (auto& mie : InstructionIterable(block)) {
      auto insn = mie.insn;
      if (!opcode::is_invoke_virtual(insn->opcode())) {
        continue;
      }
      auto mref = insn->get_method();
      if (mref != m_getClass_ref) {
        continue;
      }
      auto cfg_it = block->to_cfg_instruction_iterator(mie);
      auto move_result = cfg->move_result_of(cfg_it);
      always_assert_log(
          !move_result.is_end(),
          "[%s] %s contains spurious Object.getClass() instruction!\n  {%s}",
          get_property_name().c_str(), SHOW(method), SHOW(insn));
    }
  }
}

void NoSpuriousGetClassCallsChecker::run_checker(DexStoresVector& stores,
                                                 ConfigFiles& /* conf */,
                                                 PassManager&,
                                                 bool established) {
  if (!established) {
    return;
  }
  const auto& scope = build_class_scope(stores);
  m_getClass_ref =
      DexMethod::get_method("Ljava/lang/Object;.getClass:()Ljava/lang/Class;");
  if (m_getClass_ref == nullptr) {
    // Could not find Ljava/lang/Object;.getClass:()Ljava/lang/Class;, check
    // pass. just return;
    return;
  }
  walk::parallel::methods(
      scope, [&](DexMethod* method) { check_spurious_getClass(method); });
}

} // namespace redex_properties

namespace {
static redex_properties::NoSpuriousGetClassCallsChecker s_checker;
} // namespace
