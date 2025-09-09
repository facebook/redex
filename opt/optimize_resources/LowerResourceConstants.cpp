/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LowerResourceConstants.h"

#include "PassManager.h"
#include "Walkers.h"

void LowerResourceConstantsPass::run_pass(DexStoresVector& stores,
                                          ConfigFiles& /* unused */,
                                          PassManager& mgr) {
  Scope scope = build_class_scope(stores);
  std::atomic<size_t> lowered_instruction_count{0};
  walk::parallel::opcodes(scope,
                          [&](DexMethod* /* unused */, IRInstruction* insn) {
                            if (insn->opcode() == IOPCODE_R_CONST) {
                              insn->set_opcode(OPCODE_CONST);
                              lowered_instruction_count.fetch_add(1);
                            }
                          });
  mgr.incr_metric("lowered_r_const_instructions",
                  lowered_instruction_count.load());
}

static LowerResourceConstantsPass s_pass;
