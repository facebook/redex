/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "CopyPropagationPass.h"

#include <cinttypes>

#include "DexUtil.h"
#include "PassManager.h"

using namespace copy_propagation_impl;

void CopyPropagationPass::run_pass(DexStoresVector& stores,
                                   ConfigFiles& conf,
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
  auto stats = impl.run(scope, conf);
  mgr.incr_metric("redundant_moves_eliminated", stats.moves_eliminated);
  mgr.incr_metric("source_regs_replaced_with_representative",
                  stats.replaced_sources);
  mgr.incr_metric("method_type_inferences", stats.type_inferences);
  mgr.incr_metric("lock_fixups", stats.lock_fixups);
  mgr.incr_metric("non_singleton_lock_rdefs", stats.non_singleton_lock_rdefs);
  TRACE(RME,
        1,
        "%" PRId64 " redundant moves eliminated",
        mgr.get_metric("redundant_moves_eliminated"));
  TRACE(RME,
        1,
        "%" PRId64 " source registers replaced with representative",
        mgr.get_metric("source_regs_replaced_with_representative"));
  TRACE(RME,
        1,
        "%" PRId64 " methods had type inference computed",
        mgr.get_metric("method_type_inferences"));
}

static CopyPropagationPass s_pass;
