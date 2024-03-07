/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "InjectionIdInstructionsChecker.h"

#include "Debug.h"
#include "DexClass.h"
#include "Show.h"
#include "Walkers.h"

namespace redex_properties {

void InjectionIdInstructionsChecker::run_checker(DexStoresVector& stores,
                                                 ConfigFiles& /* conf */,
                                                 PassManager& /* mgr */,
                                                 bool established) {
  if (established) {
    return;
  }
  const auto& scope = build_class_scope(stores);
  walk::parallel::opcodes(scope, [&](DexMethod* method, IRInstruction* insn) {
    always_assert_log(!opcode::is_injection_id(insn->opcode()),
                      "[%s] %s contains injection id instruction!\n  {%s}",
                      get_name(get_property()), SHOW(method), SHOW(insn));
  });
}

} // namespace redex_properties

namespace {
static redex_properties::InjectionIdInstructionsChecker s_checker;
} // namespace
