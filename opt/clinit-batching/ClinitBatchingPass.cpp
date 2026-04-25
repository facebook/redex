/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ClinitBatchingPass.h"

#include "ConfigFiles.h"
#include "PassManager.h"
#include "Trace.h"

void ClinitBatchingPass::bind_config() {
  bind("interaction_pattern", "", m_interaction_pattern,
       "Regex pattern to filter baseline profile interactions (e.g., "
       "``ColdStart``). Only clinits hot in matching interactions are "
       "candidates for batching.");
  trait(Traits::Pass::unique, true);
}

void ClinitBatchingPass::run_pass(DexStoresVector& /* stores */,
                                  ConfigFiles& /* conf */,
                                  PassManager& mgr) {
  // Pass skeleton - implementation in subsequent tasks (T2-T6).
  mgr.set_metric("batched_clinits", 0);
  mgr.set_metric("interaction_pattern_set",
                 m_interaction_pattern.empty() ? 0 : 1);
  TRACE(CLINIT_BATCHING,
        1,
        "ClinitBatchingPass: pass skeleton (not yet implemented)");
}

static ClinitBatchingPass s_pass;
