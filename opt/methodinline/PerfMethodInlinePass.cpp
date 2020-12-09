/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PerfMethodInlinePass.h"

#include "ConfigFiles.h"
#include "InlineForSpeed.h"
#include "MethodInliner.h"
#include "MethodProfiles.h"
#include "Trace.h"

void PerfMethodInlinePass::run_pass(DexStoresVector& stores,
                                    ConfigFiles& conf,
                                    PassManager& mgr) {
  if (mgr.get_redex_options().instrument_pass_enabled) {
    TRACE(METH_PROF,
          1,
          "Skipping PerfMethodInlinePass because Instrumentation is enabled");
    return;
  }
  if (!mgr.get_redex_options().enable_pgi) {
    TRACE(METH_PROF, 1, "PerfMethodInlinePass requires --enable-pgi to run");
    return;
  }

  const auto& method_profiles = conf.get_method_profiles();
  if (!method_profiles.has_stats()) {
    // PerfMethodInline is enabled, but there are no profiles available. Bail,
    // don't run a regular inline pass.
    TRACE(METH_PROF, 1, "No profiling data available");
    return;
  }

  InlineForSpeed ifs(&method_profiles);

  inliner::run_inliner(
      stores, mgr, conf, /* intra_dex */ true, /* inline_for_speed= */ &ifs);
}

static PerfMethodInlinePass s_pass;
