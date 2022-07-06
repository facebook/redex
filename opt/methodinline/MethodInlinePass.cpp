/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodInlinePass.h"

#include "MethodInliner.h"

void MethodInlinePass::run_pass(DexStoresVector& stores,
                                ConfigFiles& conf,
                                PassManager& mgr) {
  inliner::run_inliner(stores, mgr, conf);
}

static MethodInlinePass s_pass;
