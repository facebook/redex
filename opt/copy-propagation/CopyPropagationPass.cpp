/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CopyPropagationPass.h"

#include "DexUtil.h"
#include "PassManager.h"

using namespace copy_propagation_impl;

void CopyPropagationPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& /* unused */,
                                   PassManager& mgr) {
  auto scope = build_class_scope(stores);

  if (m_config.eliminate_const_literals &&
      !mgr.get_redex_options().verify_none_enabled) {
    // This option is not safe with the verifier
    m_config.eliminate_const_literals = false;
    TRACE(RME,
          1,
          "Ignoring eliminate_const_literals because verify-none is not "
          "enabled.");
  }
  m_config.regalloc_has_run = mgr.regalloc_has_run();

  CopyPropagation impl(m_config);
  auto stats = impl.run(scope);
  mgr.incr_metric("redundant_moves_eliminated", stats.moves_eliminated);
  mgr.incr_metric("source_regs_replaced_with_representative",
                  stats.replaced_sources);
  mgr.incr_metric("methods_skipped_due_to_too_many_registers",
                  stats.skipped_due_to_too_many_registers);
  mgr.incr_metric("method_type_inferences", stats.type_inferences);
  TRACE(RME,
        1,
        "%d redundant moves eliminated",
        mgr.get_metric("redundant_moves_eliminated"));
  TRACE(RME,
        1,
        "%d source registers replaced with representative",
        mgr.get_metric("source_regs_replaced_with_representative"));
  TRACE(RME,
        1,
        "%d methods skipped due to too many registers",
        mgr.get_metric("methods_skipped_due_to_too_many_registers"));
  TRACE(RME,
        1,
        "%d methods had type inference computed",
        mgr.get_metric("method_type_inferences"));
}

static CopyPropagationPass s_pass;
