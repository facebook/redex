/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PerfMethodInlinePass.h"

#include "MethodInliner.h"

void PerfMethodInlinePass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  if (mgr.get_redex_options().instrument_pass_enabled) {
    TRACE(METH_PROF,
          1,
          "Skipping PerfMethodInlinePass because Instrumentation is enabled");
    return;
  }
  inliner::run_inliner(
      stores, mgr, conf, /* intra_dex */ true, /* use_method_profiles */ true);
}

static PerfMethodInlinePass s_pass;
