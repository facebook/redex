/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MethodInlinePass.h"

#include "MethodInliner.h"

void MethodInlinePass::run_pass(DexStoresVector& stores,
                                ConfigFiles& conf,
                                PassManager& mgr) {
  // TODO: use method profiles
  const auto& meth_profs = conf.get_method_profiles();
  inliner::run_inliner(stores, mgr, conf.get_inliner_config());
}

static MethodInlinePass s_pass;
