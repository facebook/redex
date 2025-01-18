/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "IntraDexInlinePass.h"

#include "MethodInliner.h"

void IntraDexInlinePass::bind_config() {
  bind("consider_hot_cold", false, m_consider_hot_cold);
  bind("partial_hot_hot", false, m_partial_hot_hot);
}

void IntraDexInlinePass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  inliner::run_inliner(stores, mgr, conf, DEFAULT_COST_CONFIG,
                       m_consider_hot_cold, m_partial_hot_hot,
                       /* intra_dex */ true);
  // For partial inlining, we only consider the first time the pass runs, to
  // avoid repeated partial inlining. (This shouldn't be necessary as the
  // partial inlining fallback invocation is marked as cold, but just in case
  // some other Redex optimization disturbs that hotness data.)
  if (m_partial_hot_hot) {
    m_partial_hot_hot = false;
  }
}

static IntraDexInlinePass s_pass;
