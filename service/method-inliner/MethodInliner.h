/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "Inliner.h"
#include "InlinerConfig.h"
#include "PassManager.h"

class InlineForSpeed;

namespace inliner {
/**
 * Before InterDexPass, we can run inliner with "intra_dex=false" to do global
 * inlining. But after InterDexPass, we can only run inliner within each dex by
 * setting "intra_dex" to true.
 */
void run_inliner(DexStoresVector& stores,
                 PassManager& mgr,
                 ConfigFiles& conf,
                 InlinerCostConfig inliner_cost_config = DEFAULT_COST_CONFIG,
                 bool intra_dex = false,
                 InlineForSpeed* inline_for_speed = nullptr,
                 bool inline_bridge_synth_only = false,
                 bool local_only = false);
} // namespace inliner
