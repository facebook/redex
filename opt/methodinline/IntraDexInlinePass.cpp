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
}

void IntraDexInlinePass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  inliner::run_inliner(stores, mgr, conf, DEFAULT_COST_CONFIG,
                       m_consider_hot_cold,
                       /* intra_dex */ true);
}

static IntraDexInlinePass s_pass;
